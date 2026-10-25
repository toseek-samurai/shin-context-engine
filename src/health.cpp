#include "context_internal.h"
#include "compact_terms.h"
#include "generated_snapshot.h"
#include "generated_row_decoder.h"
#include "task_request.h"
#include "snapshot_verification.h"
#include "source_read.h"

namespace shin::context {

static void add_doctor_issue(std::vector<DoctorIssue>& issues,
                      DoctorCounts& counts,
                      const std::string& severity,
                      const std::string& id,
                      const std::string& path,
                      int line,
                      const std::string& summary) {
    issues.push_back({severity, id, path, summary, line});
    if (severity == "error") {
        ++counts.errors;
    } else if (severity == "warning") {
        ++counts.warnings;
    } else {
        ++counts.infos;
    }
}

static void emit_doctor_counts_json(std::ostream& out, const DoctorCounts& counts) {
    out << "{"
        << "\"documents\":" << counts.documents
        << ",\"sections\":" << counts.sections
        << ",\"links\":" << counts.links
        << ",\"topics\":" << counts.topics
        << ",\"terms\":" << counts.terms
        << ",\"term_offsets\":" << counts.term_offsets
        << ",\"row_offsets\":" << counts.row_offsets
        << ",\"decisions\":" << counts.decisions
        << ",\"contracts\":" << counts.contracts
        << ",\"runbooks\":" << counts.runbooks
        << ",\"facts\":" << counts.facts
        << ",\"manifest_source_count\":" << counts.manifest_source_count
        << ",\"manifest_broken_local_links\":" << counts.manifest_broken_local_links
        << ",\"documents_without_summary\":" << counts.documents_without_summary
        << ",\"documents_without_topics\":" << counts.documents_without_topics
        << ",\"sections_without_preview\":" << counts.sections_without_preview
        << ",\"invalid_section_ranges\":" << counts.invalid_section_ranges
        << ",\"orphan_sections\":" << counts.orphan_sections
        << ",\"duplicate_document_ids\":" << counts.duplicate_document_ids
        << ",\"duplicate_fact_ids\":" << counts.duplicate_fact_ids
        << ",\"broken_links\":" << counts.broken_links
        << ",\"missing_agent_start_paths\":" << counts.missing_agent_start_paths
        << ",\"source_fact_candidate_files\":" << counts.source_fact_candidate_files
        << ",\"source_fact_scanned_files\":" << counts.source_fact_scanned_files
        << ",\"source_fact_skipped_large_files\":" << counts.source_fact_skipped_large_files
        << ",\"source_fact_capped_files\":" << counts.source_fact_capped_files
        << ",\"json_key_candidate_sections\":" << counts.json_key_candidate_sections
        << ",\"json_key_indexed_sections\":" << counts.json_key_indexed_sections
        << ",\"json_key_capped_sections\":" << counts.json_key_capped_sections
        << ",\"jsonl_candidate_rows\":" << counts.jsonl_candidate_rows
        << ",\"jsonl_indexed_rows\":" << counts.jsonl_indexed_rows
        << ",\"jsonl_capped_rows\":" << counts.jsonl_capped_rows
        << ",\"secret_scan_excluded_files\":" << counts.secret_scan_excluded_files
        << ",\"secret_findings\":" << counts.secret_findings
        << ",\"quarantined_files\":" << counts.quarantined_files
        << ",\"errors\":" << counts.errors
        << ",\"warnings\":" << counts.warnings
        << ",\"infos\":" << counts.infos
        << "}";
}

static void emit_doctor_issue_json(std::ostream& out, const DoctorIssue& issue) {
    out << "{"
        << "\"severity\":" << json_string(issue.severity)
        << ",\"id\":" << json_string(issue.id)
        << ",\"path\":" << json_string(issue.path)
        << ",\"line\":" << issue.line
        << ",\"summary\":" << json_string(issue.summary)
        << "}";
}

static void emit_doctor_issues_json(std::ostream& out, const std::vector<DoctorIssue>& issues, const std::string& indent) {
    for (std::size_t i = 0; i < issues.size(); ++i) {
        out << indent;
        emit_doctor_issue_json(out, issues[i]);
        if (i + 1 < issues.size()) {
            out << ",";
        }
        out << "\n";
    }
}

static bool strict_result_ok(int errors, int warnings, bool strict) {
    return errors == 0 && (!strict || warnings == 0);
}

static bool parse_strict_only_args(const std::vector<std::string>& args, const std::string& command) {
    bool strict = false;
    for (const std::string& arg : args) {
        if (arg == "--strict") {
            strict = true;
        } else {
            throw_usage_error("unsupported-option", command + " only accepts --strict.");
        }
    }
    return strict;
}

static fs::path agent_start_path_location(const RuntimeOptions& options, const std::string& path) {
    if (starts_with(path, (kGeneratedDir / "").generic_string()) || starts_with(path, "docs/kb/generated/")) {
        return options.out_root / path;
    }
    return options.root / path;
}

static const fs::path& agent_start_path_root(const RuntimeOptions& options, const std::string& path) {
    if (starts_with(path, (kGeneratedDir / "").generic_string()) || starts_with(path, "docs/kb/generated/")) {
        return options.out_root;
    }
    return options.root;
}

static void add_profile_issue(std::vector<DoctorIssue>& issues,
                       ProfileCheckCounts& counts,
                       const std::string& severity,
                       const std::string& id,
                       const std::string& path,
                       const std::string& summary) {
    issues.push_back({severity, id, path, summary, 0});
    if (severity == "error") {
        ++counts.errors;
    } else if (severity == "warning") {
        ++counts.warnings;
    } else {
        ++counts.infos;
    }
}

static void emit_profile_check_counts_json(std::ostream& out, const ProfileCheckCounts& counts) {
    out << "{"
        << "\"root_files\":" << counts.root_files
        << ",\"optional_root_files\":" << counts.optional_root_files
        << ",\"source_extensions\":" << counts.source_extensions
        << ",\"source_fact_roots\":" << counts.source_fact_roots
        << ",\"source_fact_extensions\":" << counts.source_fact_extensions
        << ",\"source_fact_extractors\":" << counts.source_fact_extractors
        << ",\"source_fact_attribute_keys\":" << counts.source_fact_attribute_keys
        << ",\"document_include_prefixes\":" << counts.document_include_prefixes
        << ",\"document_exclude_prefixes\":" << counts.document_exclude_prefixes
        << ",\"json_key_include_prefixes\":" << counts.json_key_include_prefixes
        << ",\"secret_scan_exclude_prefixes\":" << counts.secret_scan_exclude_prefixes
        << ",\"agent_start_paths\":" << counts.agent_start_paths
        << ",\"topic_rules\":" << counts.topic_rules
        << ",\"aliases\":" << counts.aliases
        << ",\"eval_cases\":" << counts.eval_cases
        << ",\"pack_eval_cases\":" << counts.pack_eval_cases
        << ",\"errors\":" << counts.errors
        << ",\"warnings\":" << counts.warnings
        << ",\"infos\":" << counts.infos
        << "}";
}

static bool extension_token_valid(const std::string& extension) {
    if (extension.size() < 2 || extension.front() != '.') {
        return false;
    }
    for (std::size_t i = 1; i < extension.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(extension[i]);
        if (!std::isalnum(ch) && extension[i] != '+' && extension[i] != '#' &&
            extension[i] != '-' && extension[i] != '_') {
            return false;
        }
    }
    return true;
}

static ProfileCheckCounts count_profile_entries(const Profile& profile) {
    ProfileCheckCounts counts;
    counts.root_files = static_cast<int>(profile.root_files.size());
    counts.optional_root_files = static_cast<int>(profile.optional_root_files.size());
    counts.source_extensions = static_cast<int>(profile.source_extensions.size());
    counts.source_fact_roots = static_cast<int>(profile.source_fact_roots.size());
    counts.source_fact_extensions = static_cast<int>(profile.source_fact_extensions.size());
    counts.source_fact_extractors = static_cast<int>(profile.source_fact_extractors.size());
    counts.source_fact_attribute_keys = static_cast<int>(profile.source_fact_attribute_keys.size());
    counts.document_include_prefixes = static_cast<int>(profile.document_include_prefixes.size());
    counts.document_exclude_prefixes = static_cast<int>(profile.document_exclude_prefixes.size());
    counts.json_key_include_prefixes = static_cast<int>(profile.json_key_include_prefixes.size());
    counts.secret_scan_exclude_prefixes = static_cast<int>(profile.secret_scan_exclude_prefixes.size());
    counts.agent_start_paths = static_cast<int>(profile.agent_start_paths.size());
    counts.topic_rules = static_cast<int>(profile.topic_rules.size());
    counts.aliases = static_cast<int>(profile.aliases.size());
    counts.eval_cases = static_cast<int>(profile.eval_cases.size());
    counts.pack_eval_cases = static_cast<int>(profile.pack_eval_cases.size());
    return counts;
}

static bool profile_prefix_valid(const std::string& prefix) {
    return profile_relative_path_valid(prefix);
}

static void validate_profile_prefix_list(const std::vector<std::string>& prefixes,
                                  const std::string& key,
                                  ProfileCheckCounts& counts,
                                  std::vector<DoctorIssue>& issues) {
    std::set<std::string> seen;
    for (const std::string& prefix : prefixes) {
        if (!profile_prefix_valid(prefix)) {
            add_profile_issue(issues, counts, "error", "invalid-profile-prefix", prefix,
                              key + " entries must be canonical relative paths without trailing slash or dot segments.");
        }
        if (!seen.insert(prefix).second) {
            add_profile_issue(issues, counts, "error", "duplicate-profile-prefix", prefix,
                              key + " contains the same path prefix more than once.");
        }
    }
}

static void validate_profile_document_policy(const Profile& profile,
                                      ProfileCheckCounts& counts,
                                      std::vector<DoctorIssue>& issues) {
    validate_profile_prefix_list(profile.document_include_prefixes, "documentIncludePrefixes", counts, issues);
    validate_profile_prefix_list(profile.document_exclude_prefixes, "documentExcludePrefixes", counts, issues);
    validate_profile_prefix_list(profile.json_key_include_prefixes, "jsonKeyIncludePrefixes", counts, issues);
    validate_profile_prefix_list(profile.secret_scan_exclude_prefixes, "secretScanExcludePrefixes", counts, issues);
    for (const std::string& included : profile.document_include_prefixes) {
        if (std::find(profile.document_exclude_prefixes.begin(), profile.document_exclude_prefixes.end(),
                      included) != profile.document_exclude_prefixes.end()) {
            add_profile_issue(issues, counts, "warning", "conflicting-document-prefix", included,
                              "The same prefix is both included and excluded; exclusion takes precedence.");
        }
    }
    if (profile.json_key_max_depth < 0 || profile.json_key_max_sections_per_file < 0 ||
        profile.jsonl_max_rows_per_file < 0) {
        add_profile_issue(issues, counts, "error", "negative-structured-data-limit", profile.config_source,
                          "JSON key and JSONL row limits must be zero or positive.");
    }
}

static void validate_profile_paths(const RuntimeOptions& options,
                            ProfileCheckCounts& counts,
                            std::vector<DoctorIssue>& issues) {
    const Profile& profile = options.profile;
    if (!fs::exists(options.root)) {
        add_profile_issue(issues, counts, "error", "missing-root", options.root.generic_string(),
                          "Profile root does not exist.");
    }
    if (profile.root_files.empty() && profile.optional_root_files.empty() &&
        profile.source_fact_roots.empty()) {
        add_profile_issue(issues, counts, "error", "profile-inputs-missing", profile.config_source,
                          "Profile must define rootFiles, optionalRootFiles, or sourceFactRoots.");
    }
    std::set<std::string> configured_root_files;
    for (const std::string& path : profile.root_files) {
        if (!profile_relative_path_valid(path)) {
            add_profile_issue(issues, counts, "error", "invalid-root-file", path,
                              "rootFiles entries must be canonical relative file paths.");
            continue;
        }
        if (!configured_root_files.insert(path).second) {
            add_profile_issue(issues, counts, "error", "duplicate-root-file", path,
                              "rootFiles entries must be unique.");
        }
        std::error_code path_ec;
        const fs::file_status status = fs::symlink_status(options.root / path, path_ec);
        if (profile_path_contains_symlink(options.root, path)) {
            add_profile_issue(issues, counts, "error", "unsafe-root-file", path,
                              "Profile rootFiles entry must not pass through a symlink.");
        } else if (path_ec || !fs::exists(status)) {
            add_profile_issue(issues, counts, "error", "missing-root-file", path,
                              "Profile rootFiles entry does not exist under --root.");
        } else if (!fs::is_regular_file(status)) {
            add_profile_issue(issues, counts, "error", "unsafe-root-file", path,
                              "Profile rootFiles entry must be a regular non-symlink file.");
        }
        if (!document_path_allowed(profile, path)) {
            add_profile_issue(issues, counts, "error", "root-file-excluded-by-document-policy", path,
                              "Profile document path policy excludes a required rootFiles entry.");
        }
    }
    for (const std::string& path : profile.optional_root_files) {
        if (!profile_prefix_valid(path)) {
            add_profile_issue(issues, counts, "error", "invalid-optional-root-file", path,
                              "optionalRootFiles entries must be canonical relative file paths.");
            continue;
        }
        if (!configured_root_files.insert(path).second) {
            add_profile_issue(issues, counts, "error", "duplicate-root-file-candidate", path,
                              "A root file path must not appear in both required and optional lists.");
        }
        if (!document_path_allowed(profile, path)) {
            add_profile_issue(issues, counts, "error", "optional-root-file-excluded-by-document-policy", path,
                              "Profile document path policy excludes an optionalRootFiles entry.");
        }
        std::error_code optional_ec;
        const fs::file_status optional_status = fs::symlink_status(options.root / path, optional_ec);
        if (profile_path_contains_symlink(options.root, path)) {
            add_profile_issue(issues, counts, "error", "unsafe-optional-root-file", path,
                              "An existing optionalRootFiles entry must not pass through a symlink.");
        } else if (!optional_ec && fs::exists(optional_status) && !fs::is_regular_file(optional_status)) {
            add_profile_issue(issues, counts, "error", "unsafe-optional-root-file", path,
                              "An existing optionalRootFiles entry must be a regular non-symlink file.");
        }
    }
    if (profile.source_fact_roots.empty() && enumerate_source_paths(profile, options.root).empty()) {
        add_profile_issue(issues, counts, "warning", "no-indexable-document-files", profile.config_source,
                          "Profile document inputs contain no existing regular files.");
    }
    for (const std::string& path : profile.agent_start_paths) {
        if (!profile_relative_path_valid(path)) {
            add_profile_issue(issues, counts, "error", "invalid-agent-start-path", path,
                              "agentStartPaths entries must be canonical relative paths.");
            continue;
        }
        std::error_code start_ec;
        const fs::file_status start_status = fs::symlink_status(agent_start_path_location(options, path), start_ec);
        if (profile_path_contains_symlink(agent_start_path_root(options, path), path)) {
            add_profile_issue(issues, counts, "error", "unsafe-agent-start-path", path,
                              "Profile agentStartPaths entry must not pass through a symlink.");
        } else if (start_ec || !fs::exists(start_status)) {
            add_profile_issue(issues, counts, "error", "missing-agent-start-path", path,
                              "Profile agentStartPaths entry does not exist.");
        } else if (!fs::is_regular_file(start_status)) {
            add_profile_issue(issues, counts, "error", "unsafe-agent-start-path", path,
                              "Profile agentStartPaths entry must be a regular non-symlink file.");
        }
    }
}

static void validate_profile_source_facts(const RuntimeOptions& options,
                                   ProfileCheckCounts& counts,
                                   std::vector<DoctorIssue>& issues) {
    const Profile& profile = options.profile;
    for (const std::string& error : profile.source_fact_config_errors) {
        add_profile_issue(issues, counts, "error", "invalid-source-fact-configuration",
                          profile.config_source, error);
    }
    std::set<std::string> source_roots;
    for (const std::string& source_root : profile.source_fact_roots) {
        if (!profile_relative_path_valid(source_root, true)) {
            add_profile_issue(issues, counts, "error", "invalid-source-fact-root", source_root,
                              "sourceFactRoots entries must be canonical relative paths; only the exact root '.' is allowed.");
        }
        if (!source_roots.insert(source_root).second) {
            add_profile_issue(issues, counts, "error", "duplicate-source-fact-root", source_root,
                              "sourceFactRoots entries must be unique.");
        }
    }
    for (const std::string& extension : profile.source_extensions) {
        if (!extension_token_valid(extension)) {
            add_profile_issue(issues, counts, "error", "invalid-source-extension", extension,
                              "sourceExtensions entries must be extension tokens such as .md or .json.");
        }
    }
    for (const std::string& extension : profile.source_fact_extensions) {
        if (!extension_token_valid(extension)) {
            add_profile_issue(issues, counts, "error", "invalid-source-fact-extension", extension,
                              "sourceFactExtensions entries must be extension tokens such as .java or .ts.");
        }
    }
    std::set<std::string> extractors;
    for (const std::string& extractor : profile.source_fact_extractors) {
        if (!source_fact_extractor_known(extractor)) {
            add_profile_issue(issues, counts, "error", "unknown-source-fact-extractor", extractor,
                              "sourceFactExtractors contains an unsupported extractor id.");
        } else if (!extractors.insert(extractor).second) {
            add_profile_issue(issues, counts, "error", "duplicate-source-fact-extractor", extractor,
                              "sourceFactExtractors entries must be unique.");
        }
    }
    std::set<std::string> attribute_keys;
    if (profile.source_fact_attribute_keys.size() > kMaxSourceFactAttributeKeys) {
        add_profile_issue(issues, counts, "error", "too-many-source-fact-attribute-keys",
                          profile.config_source,
                          "sourceFactAttributeKeys exceeds the supported key count.");
    }
    for (const std::string& key : profile.source_fact_attribute_keys) {
        bool valid = !key.empty();
        for (char value : key) {
            const unsigned char ch = static_cast<unsigned char>(value);
            valid = valid && (is_ascii_alnum(ch) || value == '_' || value == '-' ||
                              value == ':' || value == '$');
        }
        if (key.size() > kMaxSourceFactAttributeKeyBytes) {
            add_profile_issue(issues, counts, "error", "source-fact-attribute-key-too-long", key,
                              "sourceFactAttributeKeys entry exceeds the supported byte length.");
        }
        if (!valid) {
            add_profile_issue(issues, counts, "error", "invalid-source-fact-attribute-key", key,
                              "sourceFactAttributeKeys entries must be non-empty source attribute tokens.");
        } else if (!attribute_keys.insert(key).second) {
            add_profile_issue(issues, counts, "error", "duplicate-source-fact-attribute-key", key,
                              "sourceFactAttributeKeys entries must be unique.");
        }
    }
    if (!profile.source_fact_attribute_keys.empty() && !extractors.contains("retool")) {
        add_profile_issue(issues, counts, "error", "source-fact-attribute-keys-require-retool", profile.config_source,
                          "sourceFactAttributeKeys requires the retool source-fact extractor.");
    }
    for (const std::string& source_root : profile.source_fact_roots) {
        if (!profile_relative_path_valid(source_root, true)) {
            continue;
        }
        std::error_code source_root_ec;
        const fs::file_status source_root_status =
            fs::symlink_status(options.root / source_root, source_root_ec);
        if (profile_path_contains_symlink(options.root, source_root)) {
            add_profile_issue(issues, counts, "error", "unsafe-source-fact-root", source_root,
                              "sourceFactRoots entry must not pass through a symlink.");
        } else if (source_root_ec || !fs::exists(source_root_status)) {
            add_profile_issue(issues, counts, profile.bootstrap_only ? "info" : "warning",
                              profile.bootstrap_only ? "bootstrap-source-fact-root-missing" :
                                                       "missing-source-fact-root",
                              source_root,
                              "sourceFactRoots entry does not exist under --root.");
        }
    }
    if (!profile.source_fact_roots.empty() && profile.source_fact_extensions.empty()) {
        add_profile_issue(issues, counts, "error", "source-fact-extensions-missing", profile.config_source,
                          "sourceFactRoots requires sourceFactExtensions.");
    }
    if (!profile.source_fact_roots.empty() && profile.source_fact_extractors.empty()) {
        add_profile_issue(issues, counts, "error", "source-fact-extractors-missing", profile.config_source,
                          "sourceFactRoots requires at least one sourceFactExtractors entry.");
    }
    if (profile.source_fact_max_files < 0 || profile.source_fact_max_file_bytes < 0) {
        add_profile_issue(issues, counts, "error", "negative-source-fact-limit", profile.config_source,
                          "sourceFactMaxFiles and sourceFactMaxFileBytes must be zero or positive.");
    }
    if (!profile.source_fact_roots.empty() && !profile.source_fact_extensions.empty() &&
        enumerate_source_fact_paths(profile, options.root).candidate_file_count == 0 &&
        enumerate_source_paths(profile, options.root).empty()) {
        add_profile_issue(issues, counts, "warning", "no-indexable-source-files", profile.config_source,
                          "Profile source roots contain no supported files and no source documents were found.");
    }
    if (profile.markdown_max_heading_level < 1 || profile.markdown_max_heading_level > 6) {
        add_profile_issue(issues, counts, "warning", "markdown-heading-level-out-of-range", profile.config_source,
                          "markdownMaxHeadingLevel should be between 1 and 6.");
    }
    if (profile.markdown_chunk_lines < 0 || profile.markdown_chunk_min_lines < 0 ||
        profile.markdown_chunk_chars < 0) {
        add_profile_issue(issues, counts, "error", "negative-markdown-chunk-limit", profile.config_source,
                          "markdownChunkLines, markdownChunkMinLines, and markdownChunkChars must be zero or positive.");
    }
    if (profile.markdown_chunk_lines == 0 && profile.markdown_chunk_min_lines > 0) {
        add_profile_issue(issues, counts, "warning", "markdown-chunking-disabled", profile.config_source,
                          "markdownChunkLines is zero, so markdownChunkMinLines will not produce chunk rows.");
    }
}

static void validate_profile_topics(const Profile& profile,
                             ProfileCheckCounts& counts,
                             std::vector<DoctorIssue>& issues) {
    std::set<std::string> topic_ids;
    for (const TopicRule& rule : profile.topic_rules) {
        if (rule.id.empty()) {
            add_profile_issue(issues, counts, "error", "empty-topic-id", profile.config_source,
                              "topicRules entry has an empty id.");
            continue;
        }
        if (!topic_ids.insert(rule.id).second) {
            add_profile_issue(issues, counts, "error", "duplicate-topic-id", rule.id,
                              "topicRules id appears more than once.");
        }
        if (rule.keywords.empty()) {
            add_profile_issue(issues, counts, "warning", "topic-without-keywords", rule.id,
                              "topicRules entry has no keywords.");
        }
    }
    if (!profile.default_topic.empty() && topic_ids.find(profile.default_topic) == topic_ids.end()) {
        add_profile_issue(issues, counts, "warning", "default-topic-not-declared", profile.default_topic,
                          "defaultTopic is not present in topicRules.");
    }
}

static void validate_profile_aliases(const Profile& profile,
                              ProfileCheckCounts& counts,
                              std::vector<DoctorIssue>& issues) {
    std::set<std::string> alias_terms;
    for (const AliasRule& rule : profile.aliases) {
        const std::string term = trim(to_lower(rule.term));
        if (term.empty()) {
            add_profile_issue(issues, counts, "error", "empty-alias-term", profile.config_source,
                              "aliases entry has an empty term.");
            continue;
        }
        if (!alias_terms.insert(term).second) {
            add_profile_issue(issues, counts, "error", "duplicate-alias-term", rule.term,
                              "aliases term appears more than once.");
        }
        if (rule.expands.empty()) {
            add_profile_issue(issues, counts, "error", "alias-without-expansions", rule.term,
                              "aliases entry must have at least one expansion.");
        }
        if (term.size() < 2 || is_query_stop_word(term)) {
            add_profile_issue(issues, counts, "warning", "broad-alias-term", rule.term,
                              "Alias term is too broad for deterministic retrieval.");
        }
        for (const std::string& expansion : rule.expands) {
            const std::string normalized = trim(to_lower(expansion));
            if (normalized.empty() || normalized.size() < 2 || is_query_stop_word(normalized)) {
                add_profile_issue(issues, counts, "warning", "broad-alias-expansion", rule.term,
                                  "Alias expansion is too broad for deterministic retrieval: " + expansion);
            }
        }
    }
}

static void validate_compound_query_terms(const std::vector<std::string>& values,
                                          const std::string& key,
                                          bool allow_empty,
                                          ProfileCheckCounts& counts,
                                          std::vector<DoctorIssue>& issues) {
    std::set<std::string> seen;
    for (const std::string& value : values) {
        const std::string normalized = prepare_match_term(value).compact;
        if (normalized.empty() && !allow_empty) {
            add_profile_issue(issues, counts, "error", "empty-compound-query-term", key,
                              key + " entries must contain a searchable stem.");
            continue;
        }
        if (!seen.insert(normalized).second) {
            add_profile_issue(issues, counts, "error", "duplicate-compound-query-term", value,
                              key + " contains the same normalized stem more than once.");
        }
    }
}

static void validate_profile_compound_query_policy(const Profile& profile,
                                                   ProfileCheckCounts& counts,
                                                   std::vector<DoctorIssue>& issues) {
    validate_compound_query_terms(profile.compound_query_preferred_stems,
                                  "compoundQueryPreferredStems", false, counts, issues);
    validate_compound_query_terms(profile.compound_query_preferred_stem_suffixes,
                                  "compoundQueryPreferredStemSuffixes", false, counts, issues);
    validate_compound_query_terms(profile.compound_query_scope_stem_suffixes,
                                  "compoundQueryScopeStemSuffixes", true, counts, issues);
    if (!profile.compound_query_first_term_scope &&
        (!profile.compound_query_preferred_stems.empty() ||
         !profile.compound_query_preferred_stem_suffixes.empty() ||
         !profile.compound_query_scope_stem_suffixes.empty())) {
        add_profile_issue(issues, counts, "warning", "inactive-compound-query-path-policy",
                          profile.config_source,
                          "Compound-query path stems are ignored unless compoundQueryFirstTermScope is true.");
    }
}

static bool eval_rank_constraints_have_cycle(const std::vector<RankBeforeExpectation>& pairs) {
    std::map<std::string, std::vector<std::string>> edges;
    for (const RankBeforeExpectation& pair : pairs) {
        edges[pair.higher_path].push_back(pair.lower_path);
    }
    std::map<std::string, int> states;
    std::function<bool(const std::string&)> visit = [&](const std::string& path) {
        int& state = states[path];
        if (state == 1) {
            return true;
        }
        if (state == 2) {
            return false;
        }
        state = 1;
        for (const std::string& next : edges[path]) {
            if (visit(next)) {
                return true;
            }
        }
        state = 2;
        return false;
    };
    for (const auto& [path, next] : edges) {
        (void)next;
        if (visit(path)) {
            return true;
        }
    }
    return false;
}

static void validate_profile_pack_eval_cases(const RuntimeOptions& options,
                                             ProfileCheckCounts& counts,
                                             std::vector<DoctorIssue>& issues) {
    const Profile& profile = options.profile;
    for (const std::string& error : profile.pack_eval_case_config_errors) {
        add_profile_issue(issues, counts, "error", "invalid-pack-eval-field",
                          profile.config_source, error);
    }
    if (profile.pack_eval_cases.size() > kMaxPackEvalCases) {
        add_profile_issue(issues, counts, "error", "too-many-pack-eval-cases",
                          profile.config_source,
                          "packEvalCases supports at most 32 cases per profile.");
    }

    std::set<std::string> ids;
    std::set<std::string> request_ids;
    for (const PackEvalCase& item : profile.pack_eval_cases) {
        if (trim(item.id).empty()) {
            add_profile_issue(issues, counts, "error", "empty-pack-eval-id",
                              profile.config_source,
                              "packEvalCases entry has an empty id.");
            continue;
        }
        if (!ids.insert(item.id).second) {
            add_profile_issue(issues, counts, "error", "duplicate-pack-eval-id", item.id,
                              "packEvalCases id appears more than once.");
        }
        if (item.max_evidence_bytes == 0 || item.max_evidence_bytes < -1 ||
            item.max_records < -1 || item.max_compact_bytes == 0 ||
            item.max_compact_bytes < -1) {
            add_profile_issue(issues, counts, "error", "invalid-pack-eval-limit", item.id,
                              "Pack eval byte limits must be positive when supplied and record limits must be non-negative.");
        }
        try {
            const TaskRequest request = parse_task_request(item.request_document, profile);
            if (!request_ids.insert(request.request_id).second) {
                add_profile_issue(issues, counts, "error",
                                  "duplicate-pack-eval-request-id", request.request_id,
                                  "packEvalCases request_id appears more than once.");
            }
        } catch (const CliError& error) {
            add_profile_issue(issues, counts, "error", "invalid-pack-eval-request", item.id,
                              error.what());
        }

        const std::set<std::string> forbidden(item.must_not_paths.begin(),
                                              item.must_not_paths.end());
        for (const std::string& path : item.must_paths) {
            if (!profile_relative_path_valid(path) || !fs::exists(options.root / path)) {
                add_profile_issue(issues, counts, "error", "missing-pack-eval-must-path",
                                  path,
                                  "packEvalCases mustPaths entry does not exist under --root.");
            }
            if (forbidden.contains(path)) {
                add_profile_issue(issues, counts, "error",
                                  "contradictory-pack-eval-path", path,
                                  "Pack eval path must not be both required and forbidden.");
            }
        }
        for (const std::string& path : item.must_not_paths) {
            if (!profile_relative_path_valid(path)) {
                add_profile_issue(issues, counts, "error", "invalid-pack-eval-forbidden-path",
                                  path,
                                  "packEvalCases mustNotPaths entries must be repository-relative paths.");
            }
        }
    }
}

static void validate_profile_eval_cases(const RuntimeOptions& options,
                                 ProfileCheckCounts& counts,
                                 std::vector<DoctorIssue>& issues) {
    const Profile& profile = options.profile;
    for (const std::string& error : profile.eval_case_config_errors) {
        add_profile_issue(issues, counts, "error", "invalid-eval-field", profile.config_source,
                          error);
    }
    if (profile.eval_cases.size() > kMaxEvalCases) {
        add_profile_issue(issues, counts, "error", "too-many-eval-cases", profile.config_source,
                          "evalCases supports at most 128 cases per profile.");
    }
    std::size_t execution_count = profile.eval_cases.size();
    for (const EvalCase& item : profile.eval_cases) {
        execution_count += item.query_variants.size();
    }
    if (execution_count > kMaxEvalExecutions) {
        add_profile_issue(issues, counts, "error", "too-many-eval-executions", profile.config_source,
                          "evalCases supports at most 256 base and variant executions per profile.");
    }

    auto validate_query_shape = [&](const std::string& query,
                                    const std::string& source,
                                    const std::string& empty_code) {
        if (query.size() > kMaxEvalQueryBytes) {
            add_profile_issue(issues, counts, "error", "eval-query-too-large", source,
                              "Eval query text must not exceed 16384 bytes.");
            return;
        }
        QueryRequest request;
        request.query = query;
        request.terms = parse_terms(query);
        if (request.terms.empty()) {
            add_profile_issue(issues, counts, "error", empty_code, source,
                              "Eval query has no meaningful terms.");
            return;
        }
        apply_profile_query_expansion(profile, request);
        if (request.term_groups.size() > kMaxQueryGroups) {
            add_profile_issue(issues, counts, "error", "too-many-eval-query-groups", source,
                              "Eval query expands beyond 64 term groups.");
        }
        std::size_t expanded_terms = 0;
        for (const std::vector<std::string>& group : request.term_groups) {
            expanded_terms += group.size();
        }
        if (expanded_terms > kMaxExpandedQueryTerms) {
            add_profile_issue(issues, counts, "error", "too-many-eval-expanded-terms", source,
                              "Eval query expands beyond 512 profile terms.");
        }
    };

    std::set<std::string> eval_ids;
    for (const EvalCase& item : profile.eval_cases) {
        if (item.id.empty()) {
            add_profile_issue(issues, counts, "error", "empty-eval-id", profile.config_source,
                              "evalCases entry has an empty id.");
            continue;
        }
        if (!eval_ids.insert(item.id).second) {
            add_profile_issue(issues, counts, "error", "duplicate-eval-id", item.id,
                              "evalCases id appears more than once.");
        }
        validate_query_shape(item.query, item.id, "empty-eval-query");
        const bool has_assertions = !item.must_paths.empty() || !item.must_top_paths.empty() ||
                                    !item.must_not_paths.empty() || !item.expected_record_types.empty() ||
                                    item.max_matched_sections >= 0 || item.max_matched_facts >= 0 ||
                                    item.max_candidate_rows >= 0 || item.min_covered_groups >= 0 ||
                                    !item.must_rank_before.empty();
        if (!has_assertions) {
            add_profile_issue(issues, counts, "error", "empty-eval-assertions", item.id,
                              "evalCases must define at least one path, type, coverage, or candidate assertion.");
        }
        if (item.limit <= 0 || item.per_doc_limit <= 0) {
            add_profile_issue(issues, counts, "error", "invalid-eval-limit", item.id,
                              "evalCases limit and perDocLimit must be positive.");
        }
        if (item.must_top_n <= 0) {
            add_profile_issue(issues, counts, "error", "invalid-eval-must-top-n", item.id,
                              "evalCases mustTopN must be positive when supplied.");
        }
        if (item.must_top_n > item.limit) {
            add_profile_issue(issues, counts, "error", "unevaluable-eval-must-top-n", item.id,
                              "evalCases mustTopN must not exceed the case limit.");
        }
        if (item.max_matched_sections < -1 || item.max_matched_facts < -1 ||
            item.max_candidate_rows < -1 || item.min_covered_groups < -1) {
            add_profile_issue(issues, counts, "error", "invalid-eval-match-ceiling", item.id,
                              "evalCases row, candidate, and coverage bounds must be -1 or a non-negative integer.");
        }
        if (item.query_variants.size() > kMaxEvalQueryVariants) {
            add_profile_issue(issues, counts, "error", "too-many-eval-query-variants", item.id,
                              "evalCases supports at most eight deterministic query variants per case.");
        }
        if (item.must_rank_before.size() > kMaxEvalRankPairs) {
            add_profile_issue(issues, counts, "error", "too-many-eval-rank-pairs", item.id,
                              "evalCases supports at most 64 mustRankBefore pairs per case.");
        }
        std::set<std::string> query_variants;
        query_variants.insert(trim(to_lower(item.query)));
        for (const std::string& query_variant : item.query_variants) {
            const std::string normalized = trim(to_lower(query_variant));
            validate_query_shape(query_variant, item.id, "empty-eval-query-variant");
            if (!query_variants.insert(normalized).second) {
                add_profile_issue(issues, counts, "error", "duplicate-eval-query-variant", item.id,
                                  "evalCases queryVariants must be distinct from the base query and each other.");
            }
        }
        for (const std::string& path : item.must_paths) {
            if (!fs::exists(options.root / path)) {
                add_profile_issue(issues, counts, "error", "missing-eval-must-path", path,
                                  "evalCases mustPaths entry does not exist under --root.");
            }
        }
        for (const std::string& path : item.must_top_paths) {
            if (!fs::exists(options.root / path)) {
                add_profile_issue(issues, counts, "error", "missing-eval-must-top-path", path,
                                  "evalCases mustTopPaths entry does not exist under --root.");
            }
        }
        std::set<std::string> rank_pairs;
        const std::set<std::string> forbidden_paths(item.must_not_paths.begin(),
                                                    item.must_not_paths.end());
        for (const RankBeforeExpectation& pair : item.must_rank_before) {
            const std::string pair_key = pair.higher_path + "\n" + pair.lower_path;
            if (pair.higher_path.empty() || pair.lower_path.empty()) {
                add_profile_issue(issues, counts, "error", "empty-eval-rank-path", item.id,
                                  "evalCases mustRankBefore paths must not be empty.");
                continue;
            }
            if (pair.higher_path == pair.lower_path) {
                add_profile_issue(issues, counts, "error", "self-eval-rank-pair", pair.higher_path,
                                  "evalCases mustRankBefore higher and lower paths must differ.");
            }
            if (forbidden_paths.find(pair.higher_path) != forbidden_paths.end() ||
                forbidden_paths.find(pair.lower_path) != forbidden_paths.end()) {
                add_profile_issue(issues, counts, "error", "contradictory-eval-rank-pair", item.id,
                                  "evalCases mustRankBefore paths must not also appear in mustNotPaths.");
            }
            if (!rank_pairs.insert(pair_key).second) {
                add_profile_issue(issues, counts, "error", "duplicate-eval-rank-pair", item.id,
                                  "evalCases mustRankBefore pair appears more than once.");
            }
            for (const std::string* path : {&pair.higher_path, &pair.lower_path}) {
                if (!profile_relative_path_valid(*path) || !fs::exists(options.root / *path)) {
                    add_profile_issue(issues, counts, "error", "missing-eval-rank-path", *path,
                                      "evalCases mustRankBefore path does not exist under --root.");
                }
            }
        }
        if (eval_rank_constraints_have_cycle(item.must_rank_before)) {
            add_profile_issue(issues, counts, "error", "cyclic-eval-rank-constraints", item.id,
                              "evalCases mustRankBefore constraints must form an acyclic order.");
        }
        for (const std::string& path : item.must_paths) {
            if (forbidden_paths.find(path) != forbidden_paths.end()) {
                add_profile_issue(issues, counts, "error", "contradictory-eval-path", path,
                                  "evalCases path must not be both required and forbidden.");
            }
        }
        for (const std::string& path : item.must_top_paths) {
            if (forbidden_paths.find(path) != forbidden_paths.end()) {
                add_profile_issue(issues, counts, "error", "contradictory-eval-top-path", path,
                                  "evalCases top path must not also be forbidden.");
            }
        }
        for (const std::string& type : item.expected_record_types) {
            if (trim(type).empty()) {
                add_profile_issue(issues, counts, "error", "empty-eval-expected-record-type", item.id,
                                  "evalCases expectedRecordTypes entries must not be empty.");
            }
        }
    }
    if (profile.eval_cases.empty()) {
        if (profile.bootstrap_only) {
            add_profile_issue(issues, counts, "info", "bootstrap-profile-without-eval", profile.config_source,
                              "Bootstrap profiles intentionally defer project-specific retrieval eval cases.");
        } else {
            add_profile_issue(issues, counts, "warning", "missing-eval-cases", profile.config_source,
                              "Profile has no evalCases; retrieval quality is not guarded.");
        }
    }
}

ProfileCheckResult validate_profile(const RuntimeOptions& options, bool strict) {
    ProfileCheckResult result;
    result.strict = strict;
    result.counts = count_profile_entries(options.profile);
    validate_profile_paths(options, result.counts, result.issues);
    validate_profile_document_policy(options.profile, result.counts, result.issues);
    validate_profile_source_facts(options, result.counts, result.issues);
    validate_profile_topics(options.profile, result.counts, result.issues);
    validate_profile_aliases(options.profile, result.counts, result.issues);
    validate_profile_compound_query_policy(options.profile, result.counts, result.issues);
    validate_profile_eval_cases(options, result.counts, result.issues);
    validate_profile_pack_eval_cases(options, result.counts, result.issues);
    return result;
}

static void emit_profile_check_result_json(const RuntimeOptions& options, const ProfileCheckResult& result) {
    const Profile& profile = options.profile;
    const bool ok = strict_result_ok(result.counts.errors, result.counts.warnings, result.strict);
    std::cout << "{\n"
              << "  \"schema_version\": \"shin.profile-check.v1\",\n"
              << "  \"tool\": \"shin-context\",\n"
              << "  \"command\": \"profile-check\",\n"
              << "  \"profile\": " << json_string(profile.id) << ",\n"
              << "  \"root\": " << json_string(options.root.generic_string()) << ",\n"
              << "  \"profile_config\": " << json_string(profile.config_source) << ",\n"
              << "  \"strict\": " << (result.strict ? "true" : "false") << ",\n"
              << "  \"ok\": " << (ok ? "true" : "false") << ",\n"
              << "  \"counts\": ";
    emit_profile_check_counts_json(std::cout, result.counts);
    std::cout << ",\n"
              << "  \"issues\": [\n";
    emit_doctor_issues_json(std::cout, result.issues, "    ");
    std::cout << "  ]\n"
              << "}\n";
}

int run_profile_check(const RuntimeOptions& options, const std::vector<std::string>& args) {
    const bool strict = parse_strict_only_args(args, "profile-check");
    ProfileCheckResult result = validate_profile(options, strict);
    emit_profile_check_result_json(options, result);
    const bool ok = strict_result_ok(result.counts.errors, result.counts.warnings, result.strict);
    return ok ? 0 : 1;
}

static void validate_doctor_generated_files(const RuntimeOptions& options,
                                     DoctorResult& result,
                                     const fs::path&,
                                     const fs::path& manifest_path) {
    if (!fs::exists(manifest_path)) {
        add_doctor_issue(result.issues, result.counts, "error", "missing-generated-manifest",
                         manifest_path.generic_string(), 0,
                         "Generated KB manifest is missing. Run build before doctor.");
    }
    for (const auto& file : expected_generated_relative_files(options)) {
        if (!generated_file_exists(options, options.out_root / file)) {
            add_doctor_issue(result.issues, result.counts, "error", "missing-generated-file",
                             (options.out_root / file).generic_string(), 0,
                             "Generated KB output file is missing.");
        }
    }
}

static void read_doctor_manifest_counts(DoctorResult& result, const fs::path& manifest_path) {
    if (!fs::exists(manifest_path)) {
        return;
    }
    const Json manifest_json = parse_json_text(read_file(manifest_path), manifest_path.string());
    result.counts.manifest_source_count = json_int_value(manifest_json, "source_count", 0);
    result.counts.manifest_broken_local_links = json_int_value(manifest_json, "broken_local_link_count", 0);
    const Json* source_fact_counts = find_json_field(manifest_json, "source_fact_counts");
    if (source_fact_counts != nullptr) {
        result.counts.source_fact_candidate_files = json_int_value(*source_fact_counts, "candidate_files", 0);
        result.counts.source_fact_scanned_files = json_int_value(*source_fact_counts, "scanned_files", 0);
        result.counts.source_fact_skipped_large_files =
            json_int_value(*source_fact_counts, "skipped_large_files", 0);
        result.counts.source_fact_capped_files = json_int_value(*source_fact_counts, "capped_files", 0);
    }
    const Json* structured_data_counts = find_json_field(manifest_json, "structured_data_counts");
    if (structured_data_counts != nullptr) {
        result.counts.json_key_candidate_sections =
            json_int_value(*structured_data_counts, "json_key_candidate_sections", 0);
        result.counts.json_key_indexed_sections =
            json_int_value(*structured_data_counts, "json_key_indexed_sections", 0);
        result.counts.json_key_capped_sections =
            json_int_value(*structured_data_counts, "json_key_capped_sections", 0);
        result.counts.jsonl_candidate_rows =
            json_int_value(*structured_data_counts, "jsonl_candidate_rows", 0);
        result.counts.jsonl_indexed_rows =
            json_int_value(*structured_data_counts, "jsonl_indexed_rows", 0);
        result.counts.jsonl_capped_rows =
            json_int_value(*structured_data_counts, "jsonl_capped_rows", 0);
    }
}

static void scan_doctor_documents(const RuntimeOptions& options,
                           DoctorResult& result,
                           const fs::path& generated_root,
                           std::set<std::string>& document_ids) {
    const fs::path documents_path = generated_root / "documents.jsonl";
    for_each_jsonl_line_if_exists(documents_path, [&](const std::string& line) {
        SourceDoc doc = parse_document_row(line);
        ++result.counts.documents;
        if (!document_ids.insert(doc.id).second) {
            ++result.counts.duplicate_document_ids;
            add_doctor_issue(result.issues, result.counts, "error", "duplicate-document-id", doc.path, 0,
                             "Document id appears more than once: " + doc.id);
        }
        if (doc.summary.empty()) {
            ++result.counts.documents_without_summary;
        }
        if (doc.topics.empty()) {
            ++result.counts.documents_without_topics;
            add_doctor_issue(result.issues, result.counts, "warning", "document-without-topics", doc.path, 0,
                             "Document has no topic assignments.");
        }
        if (!fs::exists(options.root / doc.path) && !starts_with(doc.path, "docs/kb/generated/")) {
            add_doctor_issue(result.issues, result.counts, "error", "missing-source-document", doc.path, 0,
                             "Generated document points to a missing source file.");
        }
    });
}

static void scan_doctor_sections(DoctorResult& result,
                          const fs::path& generated_root,
                          const std::set<std::string>& document_ids) {
    for_each_jsonl_line_if_exists(generated_root / "sections.jsonl", [&](const std::string& line) {
        SectionRow section = parse_section_row(line);
        ++result.counts.sections;
        if (!section.document_id.empty() && document_ids.find(section.document_id) == document_ids.end()) {
            ++result.counts.orphan_sections;
            add_doctor_issue(result.issues, result.counts, "error", "orphan-section", section.path, section.line,
                             "Section references a document_id that is absent from documents.jsonl.");
        }
        if (section.preview.empty()) {
            ++result.counts.sections_without_preview;
        }
        if (section.start_line <= 0 || section.end_line < section.start_line ||
            section.line < section.start_line || section.line > section.end_line) {
            ++result.counts.invalid_section_ranges;
            add_doctor_issue(result.issues, result.counts, "error", "invalid-section-range",
                             section.path, section.line,
                             "Section line/start_line/end_line range is inconsistent.");
        }
    });
}

static void scan_doctor_links(const Profile& profile,
                              DoctorResult& result,
                              const fs::path& generated_root) {
    for_each_jsonl_line_if_exists(generated_root / "links.jsonl", [&](const std::string& line) {
        LinkRow link = parse_link_row(line);
        ++result.counts.links;
        if (link.status == "broken") {
            ++result.counts.broken_links;
            add_doctor_issue(result.issues, result.counts,
                             profile.bootstrap_only ? "warning" : "error",
                             "broken-local-link", link.path, link.line,
                             "Broken local link target: " + link.target);
        }
    });
}

static void scan_doctor_topics(DoctorResult& result, const fs::path& generated_root) {
    for_each_jsonl_line_if_exists(generated_root / "topics.jsonl", [&](const std::string& line) {
        (void)line;
        ++result.counts.topics;
    });
}

static void scan_doctor_row_offsets(DoctorResult& result,
                             const fs::path& generated_root,
                             const std::string& data_file,
                             int expected_rows) {
    const fs::path data_path = generated_root / data_file;
    const fs::path index_path = generated_root / (fs::path(data_file).stem().generic_string() + ".index.tsv");
    int rows = 0;
    int expected_row = 0;
    long long previous_offset = -1;
    long long last_end = 0;
    for_each_jsonl_line_if_exists(index_path, [&](const std::string& line) {
        RowOffset offset;
        ++rows;
        ++result.counts.row_offsets;
        if (!parse_row_offset_line(line, offset)) {
            add_doctor_issue(result.issues, result.counts, "error", "invalid-row-offset",
                             index_path.generic_string(), rows,
                             "Row offset entry must contain row number, byte offset, and byte length.");
            return;
        }
        if (offset.row != expected_row) {
            add_doctor_issue(result.issues, result.counts, "error", "non-contiguous-row-offset",
                             index_path.generic_string(), rows,
                             "Row offset entries must be contiguous and start at row 0.");
        }
        if (offset.offset <= previous_offset) {
            add_doctor_issue(result.issues, result.counts, "error", "unsorted-row-offset",
                             index_path.generic_string(), rows,
                             "Row offset byte positions must be strictly increasing.");
        }
        previous_offset = offset.offset;
        last_end = offset.offset + offset.bytes;
        ++expected_row;
    });
    if (rows != expected_rows) {
        add_doctor_issue(result.issues, result.counts, "error", "row-index-count-mismatch",
                         index_path.generic_string(), 0,
                         index_path.filename().generic_string() + " row count must match " + data_file +
                             " row count.");
    }
    const long long data_size = file_size_or_zero(data_path);
    if (rows > 0 && last_end != data_size) {
        add_doctor_issue(result.issues, result.counts, "error", "row-index-byte-range-mismatch",
                         index_path.generic_string(), 0,
                         index_path.filename().generic_string() + " byte ranges must cover " + data_file + ".");
    }
}

static void record_doctor_fact_id(DoctorResult& result,
                           std::set<std::string>& fact_ids,
                           const std::string& prefix,
                           const std::string& issue_id,
                           const std::string& noun,
                           const FactRow& fact) {
    if (!fact.id.empty() && !fact_ids.insert(prefix + ":" + fact.id).second) {
        ++result.counts.duplicate_fact_ids;
        add_doctor_issue(result.issues, result.counts, "error", issue_id, fact.path, fact.line,
                         noun + " id appears more than once: " + fact.id);
    }
}

static void scan_doctor_fact_rows(DoctorResult& result, const fs::path& generated_root, std::set<std::string>& fact_ids) {
    for_each_jsonl_line_if_exists(generated_root / "decisions.jsonl", [&](const std::string& line) {
        ++result.counts.decisions;
        FactRow fact = parse_decision_as_fact_row(line);
        record_doctor_fact_id(result, fact_ids, "decision", "duplicate-decision-id", "Decision", fact);
    });
    for_each_jsonl_line_if_exists(generated_root / "contracts.jsonl", [&](const std::string& line) {
        ++result.counts.contracts;
        FactRow fact = parse_typed_reference_as_fact_row(line);
        record_doctor_fact_id(result, fact_ids, "contract", "duplicate-contract-id", "Contract", fact);
    });
    for_each_jsonl_line_if_exists(generated_root / "runbooks.jsonl", [&](const std::string& line) {
        ++result.counts.runbooks;
        FactRow fact = parse_typed_reference_as_fact_row(line);
        record_doctor_fact_id(result, fact_ids, "runbook", "duplicate-runbook-id", "Runbook", fact);
    });
    for_each_jsonl_line_if_exists(generated_root / "facts.jsonl", [&](const std::string& line) {
        FactRow fact = parse_generated_fact_row(line);
        ++result.counts.facts;
        record_doctor_fact_id(result, fact_ids, "fact", "duplicate-fact-id", "Fact", fact);
    });
}

static std::vector<std::string> doctor_secret_scan_paths(const Profile& profile,
                                                  const fs::path& root,
                                                  int& excluded_files) {
    std::vector<std::string> paths = enumerate_source_paths(profile, root);
    SourceFactPathResult source_paths = enumerate_source_fact_paths(profile, root);
    for (const std::string& path : source_paths.paths) {
        append_unique_string(paths, path);
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    paths.erase(std::remove_if(paths.begin(), paths.end(), [&](const std::string& path) {
                    const bool excluded =
                        path_matches_any_profile_prefix(path, profile.secret_scan_exclude_prefixes);
                    excluded_files += excluded ? 1 : 0;
                    return excluded;
                }),
                paths.end());
    return paths;
}

static void scan_doctor_source_secrets(const RuntimeOptions& options, DoctorResult& result) {
    constexpr int kMaxSecretFindingsPerFile = 20;
    std::map<std::string, std::string> quarantined;
    const auto manifest_path = options.out_root / kGeneratedDir / "manifest.json";
    if (fs::exists(manifest_path)) {
        const auto manifest = parse_json_text(read_file(manifest_path), manifest_path.string());
        const auto* sources = find_json_field(manifest, "source_fact_sources");
        if (sources && sources->is_array()) {
            for (const auto& source : *sources) {
                if (!json_string_value(source, "quarantine_reason").empty())
                    quarantined[json_string_value(source, "path")] = json_string_value(source, "content_hash");
            }
        }
        const auto* documents = find_json_field(manifest, "sources");
        if (documents && documents->is_array()) {
            for (const auto& doc : *documents) {
                const auto path = json_string_value(doc, "path");
                if (json_string_value(doc, "quarantine_reason").empty()) quarantined.erase(path);
                else quarantined[path] = json_string_value(doc, "content_hash");
            }
        }
        if (!quarantined.empty()) {
            // A manifest annotation is not permission to suppress findings.
            // Authenticate generated files and require no searchable rows from
            // the quarantined path, independently of its claimed status.
            (void)load_verified_snapshot(options.out_root);
            for (const auto* name : {"sections", "facts", "decisions", "contracts", "runbooks", "links"}) {
                for_each_jsonl_line_if_exists(options.out_root / kGeneratedDir / (std::string(name) + ".jsonl"),
                    [&](const std::string& line) {
                        const auto row = parse_json_text(line, name);
                        quarantined.erase(json_string_value(row, "path"));
                    });
            }
            for_each_jsonl_line_if_exists(options.out_root / kGeneratedDir / "documents.jsonl",
                [&](const std::string& line) {
                    const auto row = parse_json_text(line, "documents");
                    if (json_string_value(row, "title") != "Quarantined source" ||
                        json_string_value(row, "summary") != "Source content withheld after secret detection." ||
                        json_string_value(row, "doc_type") != "quarantined" ||
                        json_string_value(row, "status") != "quarantined")
                        quarantined.erase(json_string_value(row, "path"));
                });
        }
    }
    for (const std::string& path :
         doctor_secret_scan_paths(options.profile, options.root, result.counts.secret_scan_excluded_files)) {
        const fs::path full_path = options.root / path;
        if (!fs::exists(full_path) || !fs::is_regular_file(full_path)) {
            continue;
        }
        const auto isolated = quarantined.find(path);
        const std::string isolated_content = isolated != quarantined.end() ? read_file(full_path) : std::string{};
        if (isolated != quarantined.end() && isolated->second == fnv1a64(isolated_content) &&
            source_range_secret(isolated_content).found) {
            ++result.counts.secret_findings;
            ++result.counts.quarantined_files;
            add_doctor_issue(result.issues, result.counts, "info", "source-quarantined", path, 0,
                             "Source file withheld from retrieval after secret detection. Coverage is partial; values omitted.");
            continue;
        }
        std::ifstream input(full_path, std::ios::binary);
        if (!input) {
            continue;
        }
        std::string line;
        int line_number = 0;
        int findings_in_file = 0;
        while (std::getline(input, line)) {
            ++line_number;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const SecretScanHit hit = detect_secret_like_text(line);
            if (!hit.found) {
                continue;
            }
            ++result.counts.secret_findings;
            ++findings_in_file;
            add_doctor_issue(result.issues,
                             result.counts,
                             "error",
                             "secret-like-source-content",
                             path,
                             line_number,
                             "Potential secret-like source content detected by built-in scanner: " +
                                 hit.kind + ". Value redacted.");
            if (findings_in_file >= kMaxSecretFindingsPerFile) {
                add_doctor_issue(result.issues,
                                 result.counts,
                                 "warning",
                                 "secret-scan-file-cap-hit",
                                 path,
                                 line_number,
                                 "Secret scanner stopped after the per-file finding cap.");
                break;
            }
        }
    }
}

static void validate_doctor_consistency(const RuntimeOptions& options,
                                 DoctorResult& result,
                                 const fs::path& manifest_path,
                                 const fs::path& documents_path) {
    DoctorCounts& counts = result.counts;
    const bool source_only_index = counts.documents == 0 && counts.facts > 0;
    if (counts.documents == 0 && !source_only_index) {
        add_doctor_issue(result.issues, counts, "error", "empty-document-index", "", 0,
                         "documents.jsonl has no rows.");
    }
    if (counts.sections == 0 && !source_only_index) {
        add_doctor_issue(result.issues, counts, "error", "empty-section-index", "", 0,
                         "sections.jsonl has no rows.");
    }
    if (counts.topics == 0 && !source_only_index) {
        add_doctor_issue(result.issues, counts, "warning", "empty-topic-index", "", 0,
                         "topics.jsonl has no rows.");
    }
    if (source_only_index) {
        add_doctor_issue(result.issues, counts, "info", "source-only-index", manifest_path.generic_string(), 0,
                         "The generated index is intentionally backed by source facts without source documents.");
    }
    if (counts.terms == 0) {
        add_doctor_issue(result.issues, counts, "error", "empty-term-index", "", 0,
                         "Lexical dictionary has no rows.");
    }
    if (counts.term_offsets == 0) {
        add_doctor_issue(result.issues, counts, "error", "empty-term-offset-index", "", 0,
                         "Lexical offset table has no rows.");
    }
    if (counts.terms != counts.term_offsets) {
        add_doctor_issue(result.issues, counts, "error", "term-index-count-mismatch",
                         (options.out_root / kGeneratedDir / "terms.lex").generic_string(), 0,
                         "Lexical offset and dictionary row counts must match.");
    }
    const int expected_row_offsets =
        counts.sections + counts.decisions + counts.contracts + counts.runbooks + counts.facts;
    if (counts.row_offsets != expected_row_offsets) {
        add_doctor_issue(result.issues, counts, "error", "row-index-total-count-mismatch",
                         (options.out_root / kGeneratedDir).generic_string(), 0,
                         "Generated row index entries must match indexed candidate JSONL row counts.");
    }
    if (counts.manifest_source_count != 0 && counts.manifest_source_count != counts.documents) {
        add_doctor_issue(result.issues, counts, "error", "manifest-source-count-mismatch",
                         manifest_path.generic_string(), 0,
                         "manifest source_count does not match documents.jsonl row count.");
    }
    if (counts.manifest_broken_local_links != counts.broken_links) {
        add_doctor_issue(result.issues, counts, "error", "manifest-broken-link-count-mismatch",
                         manifest_path.generic_string(), 0,
                         "manifest broken_local_link_count does not match links.jsonl.");
    }
    if (counts.source_fact_capped_files > 0) {
        add_doctor_issue(result.issues, counts, "warning", "source-fact-file-cap-hit",
                         manifest_path.generic_string(), 0,
                         "Source fact extraction hit sourceFactMaxFiles; raise the profile cap or narrow roots.");
    }
    if (counts.source_fact_skipped_large_files > 0) {
        add_doctor_issue(result.issues, counts, "info", "source-fact-large-files-skipped",
                         manifest_path.generic_string(), 0,
                         "Some source fact files exceeded sourceFactMaxFileBytes and were skipped.");
    }
    if (counts.json_key_capped_sections > 0) {
        add_doctor_issue(result.issues, counts, "info", "json-key-limit-applied",
                         manifest_path.generic_string(), 0,
                         "Profile JSON depth or per-file section limits intentionally excluded candidate keys.");
    }
    if (counts.jsonl_capped_rows > 0) {
        add_doctor_issue(result.issues, counts, "info", "jsonl-row-limit-applied",
                         manifest_path.generic_string(), 0,
                         "Profile jsonlMaxRowsPerFile intentionally excluded candidate rows.");
    }
    if (counts.secret_scan_excluded_files > 0) {
        add_doctor_issue(result.issues, counts, "info", "secret-scan-path-policy-applied",
                         options.profile.config_source, 0,
                         "Profile secretScanExcludePrefixes excluded explicitly reviewed source paths.");
    }
    if (!options.profile.source_fact_roots.empty() && counts.source_fact_candidate_files > 0 &&
        counts.source_fact_scanned_files == 0) {
        add_doctor_issue(result.issues, counts, "warning", "source-facts-configured-but-not-scanned",
                         manifest_path.generic_string(), 0,
                         "Profile configures source facts, but no source files were scanned.");
    }
    if (!options.profile.source_fact_roots.empty() && counts.source_fact_candidate_files == 0 &&
        counts.documents == 0) {
        add_doctor_issue(result.issues, counts, "warning", "source-facts-have-no-candidates",
                         manifest_path.generic_string(), 0,
                         "Bootstrap profile found neither supported source files nor source documents.");
    }
    for (const std::string& start_path : options.profile.agent_start_paths) {
        if (profile_path_contains_symlink(agent_start_path_root(options, start_path), start_path)) {
            add_doctor_issue(result.issues, counts, "error", "unsafe-agent-start-path", start_path, 0,
                             "Configured agent start path passes through a symlink.");
        } else if (!fs::exists(agent_start_path_location(options, start_path))) {
            ++counts.missing_agent_start_paths;
            add_doctor_issue(result.issues, counts, "error", "missing-agent-start-path", start_path, 0,
                             "Profile agentStartPaths entry is missing.");
        }
    }
    if (counts.documents_without_summary > std::max(3, counts.documents / 2)) {
        add_doctor_issue(result.issues, counts, "warning", "low-summary-coverage", documents_path.generic_string(), 0,
                         "More than half of indexed documents have empty summaries.");
    }
}

DoctorResult inspect_generated_kb_health(const RuntimeOptions& options, bool strict) {
    DoctorResult result;
    result.strict = strict;
    const fs::path generated_root = options.out_root / kGeneratedDir;
    const fs::path manifest_path = generated_root / "manifest.json";
    const fs::path documents_path = generated_root / "documents.jsonl";
    validate_doctor_generated_files(options, result, generated_root, manifest_path);
    read_doctor_manifest_counts(result, manifest_path);

    std::set<std::string> document_ids;
    std::set<std::string> fact_ids;
    scan_doctor_documents(options, result, generated_root, document_ids);
    scan_doctor_sections(result, generated_root, document_ids);
    scan_doctor_links(options.profile, result, generated_root);
    scan_doctor_topics(result, generated_root);
    (void)compact_lexical_index(options);
    {
        try {
            const auto manifest = parse_json_text(read_file(manifest_path), manifest_path.string());
            GeneratedInput input(options, generated_root / "terms.lex");
            CompactTerms terms(input, term_row_counts(manifest));
            terms.validate_all();
            if (terms.size() != compact_term_count(manifest))
                throw_index_error("index-corrupt", "Compact lexical term count disagrees with the manifest.");
            result.counts.terms = result.counts.term_offsets = static_cast<int>(terms.size());
        } catch (const std::exception& error) {
            add_doctor_issue(result.issues, result.counts, "error", "invalid-compact-terms",
                (generated_root / "terms.lex").generic_string(), 0, error.what());
        }
    }
    scan_doctor_fact_rows(result, generated_root, fact_ids);
    scan_doctor_row_offsets(result, generated_root, "sections.jsonl", result.counts.sections);
    scan_doctor_row_offsets(result, generated_root, "decisions.jsonl", result.counts.decisions);
    scan_doctor_row_offsets(result, generated_root, "contracts.jsonl", result.counts.contracts);
    scan_doctor_row_offsets(result, generated_root, "runbooks.jsonl", result.counts.runbooks);
    scan_doctor_row_offsets(result, generated_root, "facts.jsonl", result.counts.facts);
    scan_doctor_source_secrets(options, result);
    validate_doctor_consistency(options, result, manifest_path, documents_path);
    return result;
}

static bool doctor_result_ok(const DoctorResult& result) {
    return strict_result_ok(result.counts.errors, result.counts.warnings, result.strict);
}

static void emit_doctor_result_json(const RuntimeOptions& options, const DoctorResult& result) {
    const bool ok = doctor_result_ok(result);
    std::cout << "{\n"
              << "  \"schema_version\": \"shin.doctor.v1\",\n"
              << "  \"tool\": \"shin-context\",\n"
              << "  \"command\": \"doctor\",\n"
              << "  \"profile\": " << json_string(options.profile.id) << ",\n"
              << "  \"root\": " << json_string(options.root.generic_string()) << ",\n"
              << "  \"out_root\": " << json_string(options.out_root.generic_string()) << ",\n"
              << "  \"strict\": " << (result.strict ? "true" : "false") << ",\n"
              << "  \"ok\": " << (ok ? "true" : "false") << ",\n"
              << "  \"counts\": ";
    emit_doctor_counts_json(std::cout, result.counts);
    std::cout << ",\n"
              << "  \"issues\": [\n";
    emit_doctor_issues_json(std::cout, result.issues, "    ");
    std::cout << "  ]\n"
              << "}\n";
}

int run_doctor(const RuntimeOptions& options, const std::vector<std::string>& args) {
    const bool strict = parse_strict_only_args(args, "doctor");
    DoctorResult result = inspect_generated_kb_health(options, strict);
    emit_doctor_result_json(options, result);
    const bool ok = doctor_result_ok(result);
    return ok ? 0 : 1;
}

} // namespace shin::context
