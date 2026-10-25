#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <nlohmann/json.hpp>

namespace shin::context {

namespace fs = std::filesystem;
using Json = nlohmann::json;

#ifndef SHIN_VERSION
#define SHIN_VERSION "0.0.0-dev"
#endif

inline constexpr std::string_view kProductVersion = SHIN_VERSION;
inline const fs::path kGeneratedDir = fs::path("docs") / "kb" / "generated";
inline const std::string kLegacyGeneratorVersion = "shin-generator-v21-cpp-syntax";
inline const std::string kGeneratorVersion = "shin-generator-v22-compact-lexical";
inline constexpr std::size_t kMaxSectionIndexedTextBytes = 16384;
inline constexpr std::size_t kMaxDocumentIndexedTextBytes = 131072;
inline const std::string kMatchingMode = "field-aware-v1";
inline constexpr std::size_t kMaxIndexedCandidateRows = 20000;
inline constexpr int kCompoundQueryCoverageThreshold = 8;
inline constexpr std::size_t kMaxEvalQueryVariants = 8;
inline constexpr std::size_t kMaxEvalCases = 128;
inline constexpr std::size_t kMaxEvalExecutions = 256;
inline constexpr std::size_t kMaxEvalQueryBytes = 16384;
inline constexpr std::size_t kMaxQueryGroups = 64;
inline constexpr std::size_t kMaxExpandedQueryTerms = 512;
inline constexpr std::size_t kMaxEvalRankPairs = 64;
inline constexpr std::size_t kMaxPackEvalCases = 32;
inline constexpr std::size_t kMaxSourceFactAttributeKeys = 64;
inline constexpr std::size_t kMaxSourceFactAttributeKeyBytes = 128;

class CliError final : public std::runtime_error {
public:
    CliError(std::string code,
             std::string category,
             std::string message,
             int exit_code,
             bool retryable,
             std::string remediation,
             Json diagnostic = nullptr)
        : std::runtime_error(message),
          code_(std::move(code)),
          category_(std::move(category)),
          exit_code_(exit_code),
          retryable_(retryable),
          remediation_(std::move(remediation)),
          diagnostic_(std::move(diagnostic)) {}

    const std::string& code() const noexcept { return code_; }
    const std::string& category() const noexcept { return category_; }
    int exit_code() const noexcept { return exit_code_; }
    bool retryable() const noexcept { return retryable_; }
    const std::string& remediation() const noexcept { return remediation_; }
    const Json& diagnostic() const noexcept { return diagnostic_; }

private:
    std::string code_;
    std::string category_;
    int exit_code_ = 1;
    bool retryable_ = false;
    std::string remediation_;
    Json diagnostic_;
};

[[noreturn]] inline void throw_usage_error(
    const std::string& code,
    const std::string& message,
    const std::string& remediation = "Run `shin-context help` for the command contract.") {
    throw CliError(code, "usage", message, 2, false, remediation);
}

[[noreturn]] inline void throw_index_error(const std::string& code,
                                           const std::string& message,
                                           bool retryable = true) {
    throw CliError(code, "index", message, 1, retryable,
                   "Run `shin-context ... build`, then `check` before retrying the command.");
}

inline int parse_cli_integer(const std::string& raw, const std::string& option, int minimum) {
    int value = 0;
    const char* begin = raw.data();
    const char* end = begin + raw.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < minimum) {
        throw_usage_error("invalid-option-value",
                          option + " requires an integer greater than or equal to " + std::to_string(minimum) + ".");
    }
    return value;
}

struct TopicRule {
    std::string id;
    std::string description;
    std::vector<std::string> keywords;
    bool operator==(const TopicRule&) const = default;
};

struct AliasRule {
    std::string term;
    std::vector<std::string> expands;
    bool operator==(const AliasRule&) const = default;
};

struct RankBeforeExpectation {
    std::string higher_path;
    std::string lower_path;
    bool operator==(const RankBeforeExpectation&) const = default;
};

struct EvalCase {
    std::string id;
    std::string query;
    std::vector<std::string> query_variants;
    std::vector<std::string> must_paths;
    std::vector<std::string> must_top_paths;
    std::vector<std::string> must_not_paths;
    std::vector<std::string> expected_record_types;
    std::vector<RankBeforeExpectation> must_rank_before;
    int must_top_n = 3;
    int limit = 8;
    int per_doc_limit = 2;
    int max_matched_sections = -1;
    int max_matched_facts = -1;
    int max_candidate_rows = -1;
    int min_covered_groups = -1;
    bool operator==(const EvalCase&) const = default;
};

struct PackEvalCase {
    std::string id;
    Json request_document;
    std::vector<std::string> must_paths;
    std::vector<std::string> must_not_paths;
    bool expected_complete = true;
    bool require_serial_parallel_identity = true;
    int max_evidence_bytes = -1;
    int max_records = -1;
    int max_compact_bytes = -1;
    bool operator==(const PackEvalCase&) const = default;
};

struct Profile {
    std::string id;
    std::string source_label;
    std::string config_source;
    std::string config_hash;
    std::vector<std::string> root_files;
    std::vector<std::string> optional_root_files;
    std::vector<std::string> source_extensions = {".md", ".json"};
    std::vector<std::string> source_fact_roots;
    std::vector<std::string> source_fact_extensions;
    std::vector<std::string> source_fact_extractors = {"language"};
    std::vector<std::string> source_fact_attribute_keys;
    std::vector<std::string> source_fact_config_errors;
    std::vector<std::string> document_include_prefixes;
    std::vector<std::string> document_exclude_prefixes;
    std::vector<std::string> json_key_include_prefixes;
    std::vector<std::string> secret_scan_exclude_prefixes;
    std::vector<std::string> agent_start_paths;
    std::vector<TopicRule> topic_rules;
    std::vector<AliasRule> aliases;
    std::vector<EvalCase> eval_cases;
    std::string eval_resource_policy = "strict";
    std::vector<std::string> eval_case_config_errors;
    std::vector<PackEvalCase> pack_eval_cases;
    std::vector<std::string> pack_eval_case_config_errors;
    std::vector<std::string> compound_query_preferred_stems;
    std::vector<std::string> compound_query_preferred_stem_suffixes;
    std::vector<std::string> compound_query_scope_stem_suffixes;
    int markdown_max_heading_level = 6;
    int markdown_chunk_lines = 24;
    int markdown_chunk_min_lines = 12;
    int markdown_chunk_chars = 1800;
    int source_fact_max_files = 10000;
    int source_fact_max_file_bytes = 1048576;
    int json_key_max_depth = 0;
    int json_key_max_sections_per_file = 0;
    int jsonl_max_rows_per_file = 0;
    bool json_key_sections = true;
    bool json_path_references = true;
    bool json_schema_title = true;
    bool json_summary_keys = true;
    bool sort_topics = false;
    bool bootstrap_only = false;
    bool compound_query_first_term_scope = false;
    std::string default_topic;
    bool operator==(const Profile&) const = default;
};

class GeneratedSnapshot;
class GenerationAdmission;
class ValidationGeneration;

struct RuntimeOptions {
    fs::path root = fs::current_path();
    fs::path out_root = fs::current_path();
    fs::path profile_dir = "profiles";
    Profile profile;
    bool force_index = false;
    bool force_source_scan = false;
    std::shared_ptr<const GeneratedSnapshot> generated_snapshot;
    std::shared_ptr<const GenerationAdmission> generation_admission;
    std::shared_ptr<ValidationGeneration> validation_generation;
    // Native caller cancellation/freshness fence; cannot admit or skip a gate.
    std::function<void()> validation_checkpoint;
};

struct FileMetadata {
    std::uintmax_t byte_count = 0;
    std::int64_t modified_ticks = 0;
    std::int64_t changed_ticks = 0;
    std::uint64_t device_id = 0;
    std::uint64_t file_id = 0;

    bool operator==(const FileMetadata&) const = default;
};

struct FileFingerprint {
    FileMetadata metadata;
    std::string content_hash;
};

struct SecretScanHit {
    bool found = false;
    std::string kind;
};

struct SourceDoc {
    std::string id;
    std::string path;
    std::string kind;
    std::string doc_type;
    std::string area;
    std::string status;
    std::string title;
    std::string summary;
    std::string content;
    std::string content_hash;
    std::vector<std::string> lines;
    std::vector<std::string> topics;
    int line_count = 0;
    int byte_count = 0;
    int heading_count = 0;
    std::string evidence_kind{};
    std::string verification_level{};
    std::string quarantine_reason{};
};

struct SectionRow {
    std::string document_id;
    std::string path;
    std::string status;
    std::string authority_level;
    std::string heading;
    std::string anchor;
    std::string preview;
    std::string indexed_text;
    int indexed_text_omitted_bytes = 0;
    std::vector<std::string> topics;
    int line = 1;
    int level = 1;
    int start_line = 1;
    int end_line = 1;
    int chunk_index = 0;
    std::string row_type = "section";
    std::string evidence_kind{};
    std::string verification_level{};
    auto operator<=>(const SectionRow&) const = default;
};

struct LinkRow {
    std::string path;
    std::string kind;
    std::string text;
    std::string target;
    std::string status;
    std::string resolved_path;
    int line = 1;
    int target_start_line = 0;
    int target_end_line = 0;
    std::string target_range_origin{};
};

struct DecisionRow {
    std::string id;
    std::string path;
    std::string status;
    std::string area;
    std::string title;
    std::string summary;
    std::string decision;
    std::string authority_level;
    std::vector<std::string> topics;
    int line = 1;
    std::string evidence_kind{};
    std::string verification_level{};
};

struct TypedReferenceRow {
    std::string id;
    std::string type;
    std::string path;
    std::string status;
    std::string area;
    std::string title;
    std::string summary;
    std::string authority_level;
    std::vector<std::string> topics;
    int line = 1;
    std::string evidence_kind{};
    std::string verification_level{};
};

struct FactRow {
    std::string id;
    std::string record_type;
    std::string path;
    std::string status;
    std::string area;
    std::string title;
    std::string summary;
    std::string source;
    std::string authority_level;
    std::vector<std::string> topics;
    int line = 1;
    std::string evidence_kind{};
    std::string verification_level{};
    int start_line = 1;
    int end_line = 1;
    std::vector<std::string> identifier_terms;
    std::string symbol_name{};
    std::string span_hash{};
    std::string range_origin{};
    auto operator<=>(const FactRow&) const = default;
};

struct EvidenceProvenance {
    std::string evidence_kind;
    std::string verification_level;
};

struct SourceFactSource {
    std::string path;
    std::string content_hash;
    std::uintmax_t byte_count = 0;
    std::string status;
    FileMetadata metadata;
    bool has_metadata = false;
    std::string quarantine_reason{};
};

struct SourceFactBuildResult {
    std::vector<FactRow> rows;
    std::vector<LinkRow> relations;
    std::vector<SourceFactSource> sources;
    int candidate_file_count = 0;
    int scanned_file_count = 0;
    int skipped_large_file_count = 0;
    int capped_file_count = 0;
};

struct SourceScanCorpus {
    std::vector<SourceDoc> documents;
    SourceFactBuildResult source_facts;
    std::vector<SectionRow> sections;
    std::vector<FactRow> facts;
};

struct DocumentFactBuildResult {
    std::vector<FactRow> rows;
    int candidate_rows = 0;
    int indexed_rows = 0;
    int capped_rows = 0;
};

struct StructuredDataCounts {
    int json_key_candidate_sections = 0;
    int json_key_indexed_sections = 0;
    int json_key_capped_sections = 0;
    int jsonl_candidate_rows = 0;
    int jsonl_indexed_rows = 0;
    int jsonl_capped_rows = 0;
};

struct SourceFactPathResult {
    std::vector<std::string> paths;
    int candidate_file_count = 0;
    int capped_file_count = 0;
};

struct GeneratedOutput {
    std::map<std::string, std::string> files;
    long long generated_bytes = 0;
    int document_count = 0;
    int section_count = 0;
    int link_count = 0;
    int topic_count = 0;
    int term_count = 0;
    int decision_count = 0;
    int contract_count = 0;
    int runbook_count = 0;
    int fact_count = 0;
    int source_fact_candidate_file_count = 0;
    int source_fact_scanned_file_count = 0;
    int source_fact_skipped_large_file_count = 0;
    int source_fact_capped_file_count = 0;
    StructuredDataCounts structured_data;
    int broken_local_link_count = 0;
    int term_posting_workers = 1;
};

struct GeneratedRows {
    std::vector<SourceDoc> docs;
    std::vector<SectionRow> sections;
    std::vector<LinkRow> links;
    std::vector<FactRow> facts;
    std::vector<DecisionRow> decisions;
    std::vector<TypedReferenceRow> contracts;
    std::vector<TypedReferenceRow> runbooks;
    std::map<std::string, std::set<std::string>> topic_docs;
    std::map<std::string, std::vector<FactRow>> facts_by_doc;
    SourceFactBuildResult source_facts;
    StructuredDataCounts structured_data;
    int broken_local_link_count = 0;
};

struct SearchHit {
    std::string path;
    std::string snippet;
    int line = 1;
    int start_line = 1;
    int end_line = 1;
    int score = 0;
    std::vector<int> matched_group_indices;
    std::string status;
    std::string authority_level;
    std::string evidence_kind;
    std::string verification_level;
    std::string record_type;
};

struct IndexFreshness {
    std::string status = "unknown";
    std::string mode = "not-checked";
    std::string snapshot_id;
    bool current = false;
    bool fully_verified = false;
    bool cache_hit = false;
    int source_count = 0;
    int missing_files = 0;
    int stale_files = 0;
    int missing_sources = 0;
    int stale_sources = 0;
    int extra_sources = 0;
    int source_fact_candidate_files = 0;
    int source_fact_scanned_files = 0;
    int source_fact_skipped_large_files = 0;
    int source_fact_capped_files = 0;
    int source_fact_rows = 0;
    std::vector<std::string> notes;
};

struct ContextHit {
    SectionRow section;
    int score = 0;
    std::map<std::string, int> score_components;
    std::vector<std::string> matched_fields;
    std::vector<int> matched_group_indices;
    bool complete_query_priority = false;
};

struct FactHit {
    FactRow fact;
    int score = 0;
    std::map<std::string, int> score_components;
    std::vector<std::string> matched_fields;
    std::vector<int> matched_group_indices;
    bool complete_query_priority = false;
};

struct FieldMatchInput {
    std::string_view name;
    std::string_view text;
    int score = 0;
    bool primary = true;
    std::vector<std::string> lexical_terms;
    bool lexical_ready = false;
    std::string normalized_text;
    bool normalized_ready = false;

    FieldMatchInput(std::string_view field_name,
                    std::string_view field_text,
                    int field_score,
                    bool is_primary)
        : name(field_name), text(field_text), score(field_score), primary(is_primary) {}
};

struct PreparedMatchTerm {
    std::string raw;
    std::string normalized;
    std::string compact;
    bool allow_prefix = false;
    std::vector<std::string> phrase_keys;
};

enum class LexicalMatchKind { none, prefix, exact };

using PreparedTermGroup = std::vector<PreparedMatchTerm>;
using PreparedTermGroups = std::vector<PreparedTermGroup>;

struct TermPosting {
    std::vector<int> sections;
    std::vector<int> facts;
    std::vector<int> decisions;
    std::vector<int> contracts;
    std::vector<int> runbooks;
};

using TermPostingMap = std::unordered_map<std::string, TermPosting>;

struct RenderedTermIndex {
    std::string postings_jsonl;
    std::string offsets_tsv;
    int term_count = 0;
};

struct BuildPhaseTimings {
    std::map<std::string, std::uint64_t> peak_rss_bytes;
    double read_sources_ms = 0.0;
    double document_rows_ms = 0.0;
    double source_facts_ms = 0.0;
    double authority_rows_ms = 0.0;
    double sort_rows_ms = 0.0;
    double final_counts_ms = 0.0;
    double term_postings_ms = 0.0;
    double term_render_ms = 0.0;
    double row_index_render_ms = 0.0;
    double json_render_ms = 0.0;
    double total_generate_ms = 0.0;
};

struct CandidateRows {
    bool from_terms_index = false;
    bool term_index_coverage_complete = false;
    std::size_t discovered_row_count = 0;
    std::set<int> sections;
    std::set<int> facts;
    std::set<int> decisions;
    std::set<int> contracts;
    std::set<int> runbooks;
};

struct GeneratedPathProbe {
    bool complete = true;
    int rows_examined = 0;
    std::vector<SectionRow> sections;
    std::vector<FactRow> facts;
    CandidateRows row_ids;
};

struct MatchScore {
    int total = 0;
    int matched_groups = 0;
    std::vector<int> matched_group_indices;
    std::map<std::string, int> components;
    std::vector<std::string> matched_fields;
};

struct QueryGroupCoverage {
    int index = -1;
    std::string term;
    std::set<std::string> returned_evidence_paths;
    std::set<std::string> selected_document_paths;
    std::size_t planner_candidate_rows = 0;
    bool planner_candidate_available = false;
    std::string planner_disposition;
};

struct QueryCoverage {
    std::vector<QueryGroupCoverage> groups;
};

struct RetrievalUnitKey {
    std::string path;
    bool whole_document = false;
    std::string evidence_kind;
    std::string verification_level;
    std::string authority_level;
    std::string status;
    std::string record_key;

    bool operator<(const RetrievalUnitKey& other) const {
        return std::tie(path, whole_document, evidence_kind, verification_level, authority_level, status, record_key) <
               std::tie(other.path, other.whole_document, other.evidence_kind, other.verification_level,
                        other.authority_level, other.status, other.record_key);
    }
};

struct BriefResult {
    std::map<std::string, std::vector<ContextHit>> section_hits_by_doc;
    std::vector<FactHit> decisions;
    std::vector<FactHit> contracts;
    std::vector<FactHit> runbooks;
    std::vector<FactHit> facts;
    std::set<FactRow> seen_fact_keys;
    int matched_sections = 0;
    int matched_facts = 0;
    int deduplicated_facts = 0;
    int matched_decisions = 0;
    int matched_contracts = 0;
    int matched_runbooks = 0;
    int matched_other_facts = 0;
    bool fallback_used = false;
    bool scope_fallback_used = false;
    bool document_owner_scope_used = false;
    bool directory_owner_scope_used = false;
    bool identifier_scope_used = false;
    std::string identifier_scope_key;
    std::size_t identifier_probe_rows = 0;
    bool planner_attempted = false;
    bool query_planner_used = false;
    std::string planner_rejection_reason;
    int scope_group_index = -1;
    std::size_t candidate_rows = 0;
    bool candidate_index_used = false;
    int required_group_matches = 0;
    int total_term_groups = 0;
    int planner_candidate_groups = 0;
    int planner_indexed_groups = 0;
    int planner_covered_groups = 0;
    int planner_selected_documents = 0;
    std::vector<std::size_t> planner_candidate_rows_by_group;
    std::vector<std::string> planner_dispositions_by_group;
    std::vector<std::set<std::string>> planner_selected_document_paths_by_group;
    std::set<std::string> planner_document_paths;
    std::map<RetrievalUnitKey, std::set<int>> planner_unit_coverage;
};

enum class BriefEvidenceType {
    contract,
    decision,
    runbook,
    fact,
    section
};

struct BriefEvidenceRef {
    BriefEvidenceType type = BriefEvidenceType::section;
    std::size_t index = 0;
};

struct BriefSelection {
    std::vector<ContextHit> sections;
    std::vector<FactHit> decisions;
    std::vector<FactHit> contracts;
    std::vector<FactHit> runbooks;
    std::vector<FactHit> facts;
    std::vector<BriefEvidenceRef> ordered;
    int eligible_records = 0;
    int budgeted_out_records = 0;
};

struct RankedBriefPath {
    std::string path;
    int unique_path_rank = 0;
    int first_evidence_rank = 0;
};

struct BriefExecution {
    BriefResult retrieval;
    BriefSelection selection;
    QueryCoverage coverage;
    std::vector<RankedBriefPath> ranked_paths;
    std::vector<std::string> returned_record_types;
};

struct DoctorIssue {
    std::string severity;
    std::string id;
    std::string path;
    std::string summary;
    int line = 0;
};

struct DoctorCounts {
    int documents = 0;
    int sections = 0;
    int links = 0;
    int topics = 0;
    int terms = 0;
    int term_offsets = 0;
    int row_offsets = 0;
    int decisions = 0;
    int contracts = 0;
    int runbooks = 0;
    int facts = 0;
    int manifest_source_count = 0;
    int manifest_broken_local_links = 0;
    int documents_without_summary = 0;
    int documents_without_topics = 0;
    int sections_without_preview = 0;
    int invalid_section_ranges = 0;
    int orphan_sections = 0;
    int duplicate_document_ids = 0;
    int duplicate_fact_ids = 0;
    int broken_links = 0;
    int missing_agent_start_paths = 0;
    int source_fact_candidate_files = 0;
    int source_fact_scanned_files = 0;
    int source_fact_skipped_large_files = 0;
    int source_fact_capped_files = 0;
    int jsonl_candidate_rows = 0;
    int jsonl_indexed_rows = 0;
    int jsonl_capped_rows = 0;
    int json_key_candidate_sections = 0;
    int json_key_indexed_sections = 0;
    int json_key_capped_sections = 0;
    int secret_scan_excluded_files = 0;
    int secret_findings = 0;
    int quarantined_files = 0;
    int errors = 0;
    int warnings = 0;
    int infos = 0;
};

struct DoctorResult {
    bool strict = false;
    DoctorCounts counts;
    std::vector<DoctorIssue> issues;
};

struct ProfileCheckCounts {
    int root_files = 0;
    int optional_root_files = 0;
    int source_extensions = 0;
    int source_fact_roots = 0;
    int source_fact_extensions = 0;
    int source_fact_extractors = 0;
    int source_fact_attribute_keys = 0;
    int document_include_prefixes = 0;
    int document_exclude_prefixes = 0;
    int json_key_include_prefixes = 0;
    int secret_scan_exclude_prefixes = 0;
    int agent_start_paths = 0;
    int topic_rules = 0;
    int aliases = 0;
    int eval_cases = 0;
    int pack_eval_cases = 0;
    int errors = 0;
    int warnings = 0;
    int infos = 0;
};

struct ProfileCheckResult {
    bool strict = false;
    ProfileCheckCounts counts;
    std::vector<DoctorIssue> issues;
};

struct VacuumAction {
    std::string action;
    std::string path;
    std::string status;
    std::string summary;
    long long bytes_before = 0;
    long long bytes_after = 0;
};

struct VacuumSummary {
    int expected_files = 0;
    int existing_files = 0;
    int current_files = 0;
    int missing_files = 0;
    int stale_files = 0;
    int orphan_files = 0;
    int written_files = 0;
    int removed_files = 0;
    int unchanged_files = 0;
    int broken_local_links = 0;
    bool changes_needed = false;
    bool changes_remaining = false;
};

struct VacuumAssessment {
    VacuumSummary summary;
    std::string expected_snapshot_id;
};

struct VacuumRequest {
    bool apply = false;
    bool strict = false;
};

struct VacuumResult {
    VacuumRequest request;
    VacuumSummary summary;
    std::vector<VacuumAction> actions;
    fs::path generated_root;
};

struct DiscoveryResult {
    bool has_git = false;
    bool has_docs_dir = false;
    bool has_readme = false;
    bool has_agents = false;
    bool has_pom = false;
    bool has_gradle = false;
    bool has_package_json = false;
    bool has_cmake = false;
    bool has_makefile = false;
    bool has_dotnet_solution = false;
    bool has_retool_source_roots = false;
    int markdown_files = 0;
    int json_files = 0;
    int yaml_files = 0;
    int pom_files = 0;
    int gradle_files = 0;
    int package_json_files = 0;
    int dotnet_solution_files = 0;
    int csproj_files = 0;
    int java_files = 0;
    int kotlin_files = 0;
    int csharp_files = 0;
    int cpp_files = 0;
    int ts_files = 0;
    int rsx_files = 0;
    int retool_workflow_files = 0;
    int retool_resource_files = 0;
    int docs_files = 0;
    int indexable_docs_files = 0;
    int generated_kb_files = 0;
    int profile_files = 0;
    int max_depth_scanned = 0;
    bool indexable = false;
    bool source_only_supported = true;
    bool project_mutation_required = false;
    std::string bootstrap_state;
    std::string primary_profile;
    std::vector<std::string> build_systems;
    std::vector<std::string> project_kinds;
    std::vector<std::string> signals;
    std::vector<std::string> recommended_profiles;
    std::vector<std::string> recommended_extractors;
    std::vector<std::string> next_steps;
    std::vector<std::string> warnings;
};

using SteadyClock = std::chrono::steady_clock;

struct QueryRequest {
    std::string query;
    std::vector<std::string> terms;
    std::vector<std::vector<std::string>> term_groups;
    PreparedTermGroups prepared_term_groups;
    int limit = 10;
    int per_doc_limit = 0;
    bool require_current = false;
    std::vector<std::string> allowed_evidence_types;
};

enum class BriefProjection {
    full,
    compact
};

struct RetrievalDiagnostics {
    bool fallback_used = false;
    std::size_t candidate_rows = 0;
    bool candidate_index_used = false;
    int required_group_matches = 0;
    int total_term_groups = 0;
};

struct SearchIndexResult {
    std::vector<SearchHit> hits;
    int matched = 0;
    int matched_sections = 0;
    int matched_facts = 0;
    RetrievalDiagnostics diagnostics;
};

struct ContextIndexResult {
    std::map<std::string, std::vector<ContextHit>> section_hits_by_doc;
    std::vector<FactHit> fact_hits;
    int matched_sections = 0;
    int matched_facts = 0;
    RetrievalDiagnostics diagnostics;
};

struct TermOffset {
    long long offset = 0;
    long long bytes = 0;
};

struct RowOffset {
    int row = -1;
    long long offset = 0;
    long long bytes = 0;
};

} // namespace shin::context
