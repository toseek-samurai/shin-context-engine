#pragma once

#include "context_types.h"
#include "authority.h"
#include "retrieval_session.h"

namespace shin::context {

double elapsed_ms_since(SteadyClock::time_point start);
std::uint64_t process_peak_rss_bytes() noexcept;
std::string read_file(const fs::path& path);
constexpr bool is_ascii_upper(unsigned char c) noexcept { return c >= 'A' && c <= 'Z'; }
constexpr bool is_ascii_lower(unsigned char c) noexcept { return c >= 'a' && c <= 'z'; }
constexpr bool is_ascii_digit(unsigned char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool is_ascii_alnum(unsigned char c) noexcept {
    return is_ascii_upper(c) || is_ascii_lower(c) || is_ascii_digit(c);
}
constexpr bool is_ascii_space(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
constexpr unsigned char ascii_lower(unsigned char c) noexcept {
    return is_ascii_upper(c) ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}
std::string trim(std::string value);
std::string to_lower(std::string value);
bool contains_ascii_case_insensitive(std::string_view haystack, std::string_view lower_needle) noexcept;
bool portable_identifier_valid(std::string_view value) noexcept;
unsigned int worker_count_for_items(std::size_t item_count, std::size_t target_items_per_worker = 1);
void capture_first_exception(std::exception_ptr& first_exception,
                             std::mutex& exception_mutex,
                             std::exception_ptr candidate);

template <typename Work>
void run_parallel_workers(unsigned int workers, Work&& work) {
    std::exception_ptr first_exception;
    std::mutex exception_mutex;
    std::stop_source stop_source;
    const std::stop_token shared_stop_token = stop_source.get_token();
    {
        std::vector<std::jthread> threads;
        threads.reserve(workers);
        for (unsigned int worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker]() {
                try {
                    work(worker, shared_stop_token);
                } catch (...) {
                    capture_first_exception(first_exception, exception_mutex, std::current_exception());
                    stop_source.request_stop();
                }
            });
        }
    }
    if (first_exception) {
        std::rethrow_exception(first_exception);
    }
}

template <typename Work>
void parallel_for_indices(std::size_t item_count, Work&& work) {
    const unsigned int workers = worker_count_for_items(item_count);
    if (workers <= 1) {
        for (std::size_t index = 0; index < item_count; ++index) {
            work(index);
        }
        return;
    }
    std::atomic<std::size_t> next_index{0};
    run_parallel_workers(workers, [&](unsigned int, std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            const std::size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
            if (index >= item_count) {
                break;
            }
            work(index);
        }
    });
}

template <typename Work>
void parallel_for_worker_indices(std::size_t item_count, unsigned int workers, Work&& work) {
    if (workers <= 1) {
        for (std::size_t index = 0; index < item_count; ++index) {
            work(0, index);
        }
        return;
    }
    const std::size_t rows_per_worker = item_count / workers;
    const std::size_t remainder = item_count % workers;
    run_parallel_workers(workers, [&](unsigned int worker, std::stop_token stop_token) {
        const std::size_t begin = worker * rows_per_worker + std::min<std::size_t>(worker, remainder);
        const std::size_t end = begin + rows_per_worker + (worker < remainder ? 1 : 0);
        for (std::size_t index = begin; index < end && !stop_token.stop_requested(); ++index) {
            work(worker, index);
        }
    });
}

bool starts_with(std::string_view value, std::string_view prefix) noexcept;
bool ends_with(std::string_view value, std::string_view suffix) noexcept;
std::vector<std::string> split_lines(const std::string& text);
std::string compact_whitespace(const std::string& text);
std::string truncate_text(const std::string& text, std::size_t limit);
std::string json_escape(const std::string& value);
std::string json_string(const std::string& value);
Json make_result_document(const std::string& schema_version,
                          const std::string& command,
                          bool ok);
void emit_json_document(std::ostream& out, const Json& document);
std::string json_string_array(const std::vector<std::string>& values);
std::string json_int_array(const std::vector<int>& values);
std::string json_int_map(const std::map<std::string, int>& values);
std::string slugify(const std::string& value);
std::string fnv1a64(std::string_view text);
class Fnv1a64Hasher {
public:
    Fnv1a64Hasher();
    void update(std::string_view bytes) noexcept;
    std::string digest() const;
private:
    std::uint64_t value_;
};
FileMetadata file_metadata(const fs::path& path);
FileFingerprint fingerprint_file(const fs::path& path);
FileMetadata source_file_metadata(const fs::path& path, const std::string& operation);
FileFingerprint source_fingerprint_file(const fs::path& path, const std::string& operation);
[[noreturn]] void throw_source_observation_error(const std::string& operation,
                                                const std::error_code& error,
                                                bool missing_is_change = true);
[[noreturn]] void throw_source_observation_io_error(const std::string& operation);
[[noreturn]] void throw_source_changed_during_verification(const std::string& operation);
std::string profile_config_hash(const Profile& profile);
int markdown_heading_level(const std::string& line);
Json parse_json_text(const std::string& text, const std::string& source);
bool try_parse_json_text(const std::string& text, Json& out);
Json parse_json_line(const std::string& line, const std::string& source);
const Json* find_json_field(const Json& object, const std::string& key);
const Json* find_json_field_any(const Json& object, const std::vector<std::string>& keys);
std::string json_string_value(const Json& object, const std::string& key);
std::string json_string_any(const Json& object, const std::vector<std::string>& keys);
int json_int_value(const Json& object, const std::string& key, int fallback = 0);
std::vector<int> json_int_array_value(const Json& object, const std::string& key);
int json_int_any(const Json& object, const std::vector<std::string>& keys, int fallback);
bool json_bool_any(const Json& object, const std::vector<std::string>& keys, bool fallback);
std::vector<std::string> json_string_array_value(const Json& object, const std::string& key);
std::vector<std::string> json_string_array_any(const Json& object, const std::vector<std::string>& keys);
std::vector<TopicRule> extract_topic_rules(const Json& root);
std::vector<AliasRule> extract_alias_rules(const Json& root);
std::vector<EvalCase> extract_eval_cases(const Json& root,
                                         std::vector<std::string>& configuration_errors);
std::string title_from_path(const std::string& path);
std::string extension_of(const std::string& path);
bool extension_allowed(const std::vector<std::string>& extensions, const std::string& path);
std::string basename_of(const std::string& path);
bool contains_any(const std::string& lower_text, const std::vector<std::string>& terms);
void append_unique_string(std::vector<std::string>& values, const std::string& value);
SecretScanHit detect_secret_like_text(const std::string& text, bool allow_source_suppression = true);
std::string redact_sensitive_text(const std::string& text);
std::string detect_area(const std::string& path);
std::string detect_doc_type(const std::string& path,
                            const std::string& format,
                            const std::string& title,
                            const std::string& content);
int first_heading_line(const std::vector<std::string>& lines);
std::string detect_status(const std::string& path, const std::string& content, const Json* json_doc);
std::string preview_from_lines(const std::vector<std::string>& lines,
                               std::size_t start,
                               int stop_markdown_level,
                               std::size_t limit);
std::string preview_from_line_range(const std::vector<std::string>& lines,
                                    std::size_t start,
                                    std::size_t end,
                                    std::size_t max_chars);
std::string extract_json_key_line(const std::string& line);

Profile make_profile(const std::string& id, const fs::path& profile_dir);
std::optional<std::string> find_existing_adoption_marker(const fs::path& root);
std::vector<std::string> detect_topics(const Profile& profile,
                                       const std::string& path,
                                       const std::string& title,
                                       const std::string& content);
bool path_matches_any_profile_prefix(const std::string& path, const std::vector<std::string>& prefixes);
bool document_path_allowed(const Profile& profile, const std::string& path);
bool profile_relative_path_valid(const std::string& path, bool allow_dot = false);
bool profile_path_contains_symlink(const fs::path& root, const std::string& relative_path);
std::string normalize_repository_file_argument(const fs::path& root,
                                               const std::string& value,
                                               const std::string& option);
bool shin_internal_control_path(const std::string& path);
std::vector<std::string> enumerate_source_paths(const Profile& profile, const fs::path& root);
std::vector<SourceDoc> read_sources(const Profile& profile, const fs::path& root);

std::vector<SectionRow> build_sections(const Profile& profile,
                                       const SourceDoc& doc,
                                       StructuredDataCounts* structured_counts = nullptr);
std::vector<LinkRow> build_links(const Profile& profile, const fs::path& root, const SourceDoc& doc);
Json retool_generation_environment(const fs::path& root, const std::vector<std::string>& paths);
DocumentFactBuildResult build_fact_rows(const Profile& profile, const SourceDoc& doc);
FactRow query_fact_row(const DecisionRow& row);
FactRow query_fact_row(const TypedReferenceRow& row);
EvidenceProvenance infer_document_evidence_provenance(const SourceDoc& doc);
EvidenceProvenance infer_record_evidence_provenance(const std::string& path,
                                                    const std::string& doc_type,
                                                    const std::string& record_type,
                                                    const std::string& status,
                                                    const std::string& authority_level,
                                                    bool source_fact);
bool evidence_kind_valid(std::string_view value) noexcept;
bool verification_level_valid(std::string_view value) noexcept;
SourceFactPathResult enumerate_source_fact_paths(const Profile& profile, const fs::path& root);
SourceFactBuildResult build_source_fact_rows(const Profile& profile, const fs::path& root);
SourceScanCorpus build_source_scan_corpus(const Profile& profile, const fs::path& root);
void add_source_fact_row(std::vector<FactRow>& rows,
                         const Profile& profile,
                         const std::string& path,
                         int line,
                         const std::string& language,
                         const std::string& kind,
                         const std::string& name,
                         const std::string& declaration,
                         const std::string& status = "active",
                         const std::string& authority_level = "source-fact",
                         int end_line = -1);
void enrich_language_source_facts(const Profile& profile,
                                  const std::string& path,
                                  const std::string& language,
                                  const std::vector<std::string>& lines,
                                  std::vector<FactRow>& rows);
bool source_fact_extractor_known(std::string_view extractor) noexcept;
bool should_skip_source_fact_dir(const std::string& name);
struct SourceRelationInputs;
void inspect_source_fact_content(const Profile& profile, const fs::path& root,
                                 const std::string& path, const std::string& content,
                                 std::vector<FactRow>& rows, SourceRelationInputs& relations);
void apply_syntax_source_facts(const Profile& profile, const std::string& path,
                               const std::string& content, const std::vector<std::string>& lines,
                               const std::string& language, std::vector<FactRow>& rows,
                               SourceRelationInputs& relations);
void scan_retool_source_facts(const Profile& profile,
                              const fs::path& root,
                              const std::string& path,
                              const std::string& content,
                              const std::vector<std::string>& lines,
                              std::vector<FactRow>& rows,
                              SourceRelationInputs& relations);
void finalize_retool_source_facts(const Profile& profile, std::vector<FactRow>& rows);
DiscoveryResult discover_project(const fs::path& root);

GeneratedOutput generate_outputs(const Profile& profile,
                                 const fs::path& root,
                                 BuildPhaseTimings* timings = nullptr, bool compact_terms = true);
int run_build(const RuntimeOptions& options);
int run_check(const RuntimeOptions& options);
int run_vacuum(const RuntimeOptions& options, const std::vector<std::string>& args);
// Caller holds the normal Core shared publication lock for options.out_root.
VacuumAssessment inspect_generated_kb_vacuum(const RuntimeOptions& options);
long long file_size_or_zero(const fs::path& path);
bool index_term_has_signal(std::string_view term);

PreparedTermGroups prepare_term_groups(const std::vector<std::vector<std::string>>& term_groups);
PreparedMatchTerm prepare_match_term(std::string_view term);
std::string normalize_match_text(std::string_view text);
std::vector<std::string> lexical_match_terms(std::string_view text, bool sorted = true);
std::vector<FieldMatchInput> section_match_fields(const SectionRow& row);
bool section_has_non_body_query_match(const SectionRow& row, const PreparedTermGroups& groups,
                                     bool include_document_identity = true);
std::vector<FieldMatchInput> fact_match_fields(const FactRow& row);
bool identifier_word_boundary(std::string_view text, std::size_t index);
std::string render_document_row_index(const GeneratedRows& rows);
std::set<std::string> whole_document_paths(const RuntimeOptions& options, const Json* manifest = nullptr);
RetrievalUnitKey retrieval_unit_key(const SectionRow& row, const std::set<std::string>& documents);
RetrievalUnitKey retrieval_unit_key(const FactRow& row, const std::set<std::string>& documents);
std::optional<std::vector<CandidateRows>> filter_document_coverage_candidates(
    const RuntimeOptions& options, const std::vector<CandidateRows>& groups, int required,
    const std::set<std::string>& documents, const CandidateRows* ordinary = nullptr);
// Recovery reads one complete group at a time, before the ordinary row cutoff.
// The loader omits independent source facts; only declared document units qualify.
std::optional<std::vector<CandidateRows>> filter_document_coverage_postings(
    const RuntimeOptions& options, std::size_t group_count,
    const std::function<CandidateRows(std::size_t)>& load_group, int required,
    const std::set<std::string>& documents, const CandidateRows* ordinary);
std::optional<std::vector<CandidateRows>> recover_document_coverage_candidates(
    const RuntimeOptions& options, const PreparedTermGroups& groups, int required,
    const std::set<std::string>& documents, const CandidateRows* ordinary);
void finalize_planner_unit_coverage(BriefExecution& execution);
bool can_prefix_complete_lookup_term(std::string_view term);
bool field_contains_term(FieldMatchInput& field, const PreparedMatchTerm& term);
LexicalMatchKind field_match_kind(FieldMatchInput& field, const PreparedMatchTerm& term);
bool is_query_stop_word(const std::string& term);
std::vector<std::string> parse_terms(const std::string& query);
std::vector<std::vector<std::string>> expand_query_term_groups(const Profile& profile,
                                                               const std::vector<std::string>& terms);
QueryRequest make_command_query_request(const Profile& profile,
                                        const std::vector<std::string>& args,
                                        int default_limit,
                                        int default_per_doc);
void apply_profile_query_expansion(const Profile& profile, QueryRequest& request);
int strict_required_group_matches(const QueryRequest& request);
int relaxed_required_group_matches(const QueryRequest& request);
bool can_relax_query(const QueryRequest& request);
bool should_expand_query_coverage(const QueryRequest& request, std::size_t distinct_path_count);
RetrievalDiagnostics make_retrieval_diagnostics(const QueryRequest& request,
                                                int required_group_matches,
                                                bool fallback_used);
QueryCoverage make_query_coverage(const QueryRequest& request);
void add_query_coverage_evidence(QueryCoverage& coverage,
                                 std::string_view path,
                                 const std::vector<int>& matched_group_indices);
void merge_planner_query_coverage(QueryCoverage& coverage, const BriefResult& result);
int query_coverage_covered_groups(const QueryCoverage& coverage);
Json query_coverage_json(const QueryCoverage& coverage);
Json make_compact_brief_json(const RuntimeOptions& options,
                             const QueryRequest& request,
                             const std::string& index_mode,
                             const IndexFreshness& freshness,
                             const BriefExecution& execution);
MatchScore score_section_match(const SectionRow& section,
                               const PreparedTermGroups& term_groups,
                               int required_group_matches);
MatchScore score_fact_match(const FactRow& fact,
                            const PreparedTermGroups& term_groups,
                            int required_group_matches);
bool context_hit_better(const ContextHit& a, const ContextHit& b);
bool fact_hit_better(const FactHit& a, const FactHit& b);
void push_top_search_hit(std::vector<SearchHit>& hits, const SearchHit& hit, int limit);
void push_top_context_hit(std::vector<ContextHit>& hits, const ContextHit& hit, int limit);
void push_top_fact_hit(std::vector<FactHit>& hits, const FactHit& hit, int limit);
std::string brief_fact_group(const FactRow& fact);
bool complete_brief_match_priority(const BriefResult& result, const MatchScore& match);
void push_brief_section_match(BriefResult& result,
                              const SectionRow& section,
                              const MatchScore& match,
                              int per_doc_limit);
void push_brief_fact_match(BriefResult& result,
                           const FactRow& fact,
                           const MatchScore& match,
                           int limit,
                           int per_doc_limit);
std::vector<ContextHit> select_context_sections(
    const std::map<std::string, std::vector<ContextHit>>& hits_by_doc,
    int limit,
    int per_doc_limit,
    bool prefer_distinct_paths = false);
BriefSelection select_brief_evidence(const BriefResult& result,
                                     int limit,
                                     int per_doc_limit,
                                     const std::vector<std::string>& allowed_evidence_types = {},
                                     bool prefer_distinct_paths = false);
BriefResult collect_brief_from_index_first_term_scope(const RuntimeOptions& options,
                                                       const QueryRequest& request,
                                                       const Json* manifest = nullptr);
BriefResult collect_brief_from_index_document_owner_scope(const RuntimeOptions& options,
                                                           const QueryRequest& request,
                                                           const Json* manifest = nullptr);
BriefResult collect_brief_from_index_document_coverage_plan(const RuntimeOptions& options,
                                                             const QueryRequest& request,
                                                             bool preserve_row_ranking,
                                                             int required_group_matches,
                                                             bool missing_documents_only,
                                                             const std::vector<CandidateRows>* prepared_groups = nullptr,
                                                             const Json* manifest = nullptr,
                                                             bool include_near_complete_sections = false);
BriefResult collect_brief_from_index_with_fallback(const RuntimeOptions& options,
                                                    const QueryRequest& request,
                                                    const Json* manifest = nullptr);
void add_related_document(std::vector<std::string>& related_documents,
                          std::set<std::string>& seen_documents,
                          const std::string& path);
Json section_hit_json(const ContextHit& hit);
Json fact_hit_json(const FactHit& hit);
void emit_section_hit_json(std::ostream& out, const ContextHit& hit);
void emit_fact_hit_json(std::ostream& out, const FactHit& hit);
void emit_section_hit_array(std::ostream& out,
                            const std::vector<ContextHit>& hits,
                            const std::string& indent);
void emit_fact_hit_array(std::ostream& out,
                         const std::vector<FactHit>& hits,
                         const std::string& indent);
SearchIndexResult collect_search_from_index_with_fallback(const RuntimeOptions& options,
                                                          const QueryRequest& request);
ContextIndexResult collect_context_from_index_with_fallback(const RuntimeOptions& options,
                                                            const QueryRequest& request);
int run_search(const RuntimeOptions& options, const std::vector<std::string>& args);

bool compact_lexical_manifest(const Json& manifest);
bool compact_lexical_index(const RuntimeOptions& options);
std::vector<std::string> expected_generated_relative_files(bool compact = true);
std::vector<std::string> expected_generated_relative_files(const RuntimeOptions& options);
bool generated_index_available(const RuntimeOptions& options);
IndexFreshness assess_index_freshness(const RuntimeOptions& options, bool require_current);
std::uint64_t generation_execution_count();
IndexFreshness assess_source_discovery_freshness(const RuntimeOptions& options);
void prepare_freshness_cache(const RuntimeOptions& options);
void require_current_index_or_throw(const IndexFreshness& freshness);
Json index_freshness_json(const IndexFreshness& freshness);
void emit_index_freshness_json(std::ostream& out, const IndexFreshness& freshness);
bool should_use_generated_index(const RuntimeOptions& options);
int generated_index_source_count(const RuntimeOptions& options);
IndexFreshness source_scan_freshness(const SourceScanCorpus& corpus);
void require_complete_source_scan_or_throw(const IndexFreshness& freshness);
CandidateRows load_generated_query_candidates(const RuntimeOptions& options,
                                               const PreparedTermGroups& term_groups,
                                               int required_group_matches);
CandidateRows select_generated_group_candidates(const std::vector<CandidateRows>& groups, int required);
std::vector<CandidateRows> load_generated_query_group_candidates(
    const RuntimeOptions& options,
    const PreparedTermGroups& term_groups);
// Readers return decoded candidates, or every bounded row on scan fallback.
// Only the shared scorer may reject matches; raw JSON is not lexical evidence.
void for_each_generated_section_matching_terms(
    const RuntimeOptions& options,
    const CandidateRows& candidates,
    const PreparedTermGroups& term_groups,
    int required_group_matches,
    const std::function<void(const SectionRow&)>& handle);
void for_each_generated_fact_matching_terms(
    const RuntimeOptions& options,
    const CandidateRows& candidates,
    const PreparedTermGroups& term_groups,
    int required_group_matches,
    const std::function<void(const FactRow&)>& handle);
GeneratedPathProbe probe_generated_path(const RuntimeOptions& options,
                                        const std::string& path,
                                        std::size_t max_rows_per_store = 512);
void for_each_jsonl_line_if_exists(const fs::path& path,
                                   const std::function<void(const std::string&)>& handle);
SectionRow parse_section_row(const std::string& line);
SourceDoc parse_document_row(const std::string& line);
LinkRow parse_link_row(const std::string& line);
FactRow parse_generated_fact_row(const std::string& line);
FactRow parse_decision_as_fact_row(const std::string& line);
FactRow parse_typed_reference_as_fact_row(const std::string& line);
bool parse_term_offset_line(const std::string& line, std::string& term, TermOffset& out);
bool parse_row_offset_line(const std::string& line, RowOffset& out);
[[noreturn]] void throw_file_changed_during_verification(const fs::path& path);

int run_profile_check(const RuntimeOptions& options, const std::vector<std::string>& args);
ProfileCheckResult validate_profile(const RuntimeOptions& options, bool strict);
int run_adoption_check(const fs::path& root,
                       const fs::path& profile_dir,
                       const std::vector<std::string>& args);
int run_adoption_plan(const fs::path& root,
                      const fs::path& profile_dir,
                      const std::optional<std::string>& candidate_profile_id,
                      const std::vector<std::string>& args);
int run_doctor(const RuntimeOptions& options, const std::vector<std::string>& args);
DoctorResult inspect_generated_kb_health(const RuntimeOptions& options, bool strict);
int run_context(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_brief(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_inspect(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_read(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_pack(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_pack_diff(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_pack_bench(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_pack_eval(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_batch(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_diagnose(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_worktree_diff(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_impact(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_checkpoint(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_resume(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_execution_evidence(const RuntimeOptions& options, const std::vector<std::string>& args, const std::string& command);
int run_snapshot_diff(const std::vector<std::string>& args);
int run_federated_brief(const std::vector<std::string>& args);
int run_federated_eval(const std::vector<std::string>& args);
int run_suggest_profile(const std::vector<std::string>& args);
int run_index_list(const std::vector<std::string>& args);
int run_index_prune(const std::vector<std::string>& args);
int run_bench(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_eval(const RuntimeOptions& options, const std::vector<std::string>& args);
int run_validate(const RuntimeOptions& options,
                 const fs::path& profile_dir,
                 const std::vector<std::string>& args);
int run_ready(const RuntimeOptions& options,
              const fs::path& profile_dir,
              const std::vector<std::string>& args);
int run_discover(const RuntimeOptions& options);
int run(int argc, char** argv);
std::string detect_command_name(int argc, char** argv);
void emit_error_result_noexcept(std::string_view command,
                                std::string_view code,
                                std::string_view category,
                                std::string_view message,
                                int exit_code,
                                bool retryable,
                                std::string_view remediation,
                                const Json& diagnostic = nullptr) noexcept;


} // namespace shin::context
