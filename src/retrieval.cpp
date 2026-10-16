#include "context_internal.h"
#ifdef SHIN_EXPERIMENT_AGGREGATE_RECOVERY
#include "aggregate_recovery_experiment.h"
#endif
#ifdef SHIN_EXPERIMENT_EVAL_BATCH
#include "eval_batch_route_probe.h"
#endif
#ifdef SHIN_EXPERIMENT_FIELD_PREPARATION
#include "field_preparation_experiment.h"
#endif
#ifdef SHIN_EXPERIMENT_ROW_BATCH
#include "row_batch_experiment.h"
#endif

namespace shin::context {

static bool is_query_token_body_char(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_' || c == '-' || c == '.' || c == '/' || c == '+' || c == '#';
}

bool identifier_word_boundary(std::string_view text, std::size_t index) {
    if (index == 0 || index >= text.size() || !is_ascii_upper(static_cast<unsigned char>(text[index]))) return false;
    const auto previous = static_cast<unsigned char>(text[index - 1]);
    return is_ascii_lower(previous) || is_ascii_digit(previous) ||
           (is_ascii_upper(previous) && index + 1 < text.size() &&
            is_ascii_lower(static_cast<unsigned char>(text[index + 1])));
}

static std::string normalize_query_token(const std::string& raw) {
    std::string separated;
    separated.reserve(raw.size() + 4);
    for (std::size_t index = 0; index < raw.size(); ++index) {
        if (identifier_word_boundary(raw, index) &&
            !separated.empty() && separated.back() != '-') {
            separated.push_back('-');
        }
        separated.push_back(raw[index]);
    }
    const std::string lower = to_lower(separated);
    std::size_t begin = 0;
    std::size_t end = lower.size();
    while (begin < end && !is_query_token_body_char(lower[begin])) {
        ++begin;
    }
    while (end > begin && !is_query_token_body_char(lower[end - 1])) {
        --end;
    }
    return lower.substr(begin, end - begin);
}

bool is_query_stop_word(const std::string& term) {
    static const std::set<std::string> stop_words = {
        "a", "about", "an", "and", "are", "as", "at", "be", "been", "being", "by", "can",
        "could", "did", "do", "does", "for", "from", "how", "i", "in", "into", "is", "it",
        "me", "my", "of", "on", "or", "our", "please", "run", "show", "tell", "that", "the",
        "them", "these", "this", "those", "through", "to", "us", "was", "we", "were", "what", "when",
        "where", "which", "who", "why", "will", "with", "without", "would", "you", "your"
    };
    return stop_words.find(term) != stop_words.end();
}

static std::vector<std::string> qualified_identifier_terms(const std::string& raw,
                                                          const std::string& term) {
    if (term.find("::") == std::string::npos || raw.find_first_of("<>") != std::string::npos) {
        return {};
    }
    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin < term.size()) {
        const std::size_t separator = term.find("::", begin);
        const std::size_t end = separator == std::string::npos ? term.size() : separator;
        const std::string component = term.substr(begin, end - begin);
        // Camel-case normalization has already introduced hyphens in identifiers.
        if (component.empty() ||
            !(is_ascii_lower(static_cast<unsigned char>(component.front())) ||
              component.front() == '_') ||
            !std::all_of(component.begin(), component.end(), [](char c) {
                return is_ascii_alnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
            })) {
            return {};
        }
        components.push_back(component);
        if (separator == std::string::npos) break;
        begin = separator + 2;
    }
    return components;
}

std::vector<std::string> parse_terms(const std::string& query) {
    std::vector<std::string> raw_terms;
    std::vector<std::string> meaningful_terms;
    std::istringstream input(query);
    std::string raw;
    while (input >> raw) {
        const std::string term = normalize_query_token(raw);
        if (term.empty()) {
            continue;
        }
        const auto components = qualified_identifier_terms(raw, term);
        if (!components.empty()) {
            // A qualifier such as "is" or "as" is not a natural-language stop word.
            for (const auto& component : components) {
                append_unique_string(raw_terms, component);
                append_unique_string(meaningful_terms, component);
            }
            continue;
        }
        append_unique_string(raw_terms, term);
        if (!is_query_stop_word(term)) {
            append_unique_string(meaningful_terms, term);
        }
    }
    if (!meaningful_terms.empty()) {
        return meaningful_terms;
    }
    return raw_terms;
}

static void push_unique_term(std::vector<std::string>& values, const std::string& raw) {
    const std::string value = trim(to_lower(raw));
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

static bool is_ascii_alpha_term(const std::string& term) {
    return !term.empty() && std::all_of(term.begin(), term.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
}

static bool is_ascii_vowel(char c) {
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower == 'a' || lower == 'e' || lower == 'i' || lower == 'o' || lower == 'u';
}

static void push_regular_plural_variant(std::vector<std::string>& variants,
                                        const std::string& term) {
    if (term.size() < 3 || !is_ascii_alpha_term(term)) {
        return;
    }
    if (ends_with(term, "is")) {
        push_unique_term(variants, term.substr(0, term.size() - 2) + "es");
    } else if (ends_with(term, "y") && term.size() > 1 &&
               !is_ascii_vowel(term[term.size() - 2])) {
        push_unique_term(variants, term.substr(0, term.size() - 1) + "ies");
    } else if (ends_with(term, "s") || ends_with(term, "x") || ends_with(term, "z") ||
               ends_with(term, "ch") || ends_with(term, "sh")) {
        push_unique_term(variants, term + "es");
    } else {
        push_unique_term(variants, term + "s");
    }
}

static std::vector<std::string> automatic_term_variants(const std::string& term) {
    std::vector<std::string> variants;
    push_unique_term(variants, term);
    bool singular_variant_added = false;
    if (term.size() > 4 && ends_with(term, "ies")) {
        push_unique_term(variants, term.substr(0, term.size() - 3) + "y");
        singular_variant_added = true;
    } else if (term.size() > 4 &&
               (ends_with(term, "ses") || ends_with(term, "xes") || ends_with(term, "zes") ||
                ends_with(term, "ches") || ends_with(term, "shes"))) {
        push_unique_term(variants, term.substr(0, term.size() - 2));
        singular_variant_added = true;
    } else if (term.size() > 4 && ends_with(term, "s") && !ends_with(term, "ss") &&
               !ends_with(term, "us") && !ends_with(term, "is")) {
        push_unique_term(variants, term.substr(0, term.size() - 1));
        singular_variant_added = true;
    }
    if (!singular_variant_added) {
        push_regular_plural_variant(variants, term);
    }
    std::string spaced = term;
    std::replace(spaced.begin(), spaced.end(), '-', ' ');
    std::replace(spaced.begin(), spaced.end(), '_', ' ');
    push_unique_term(variants, spaced);
    std::string hyphenated = term;
    std::replace(hyphenated.begin(), hyphenated.end(), ' ', '-');
    std::replace(hyphenated.begin(), hyphenated.end(), '_', '-');
    push_unique_term(variants, hyphenated);
    std::string underscored = term;
    std::replace(underscored.begin(), underscored.end(), ' ', '_');
    std::replace(underscored.begin(), underscored.end(), '-', '_');
    push_unique_term(variants, underscored);
    std::string compact;
    for (char c : term) {
        if (c != '-' && c != '_' && !std::isspace(static_cast<unsigned char>(c))) {
            compact.push_back(c);
        }
    }
    push_unique_term(variants, compact);
    return variants;
}

static void add_compound_identifier_query_variants(
    const std::vector<std::string>& terms,
    std::vector<std::vector<std::string>>& groups) {
    constexpr std::size_t kMaxCompoundIdentifierSegments = 8;
    if (terms.size() < 2 || groups.size() != terms.size()) {
        return;
    }

    std::vector<std::string> segments;
    segments.reserve(terms.size());
    for (const std::string& term : terms) {
        const PreparedMatchTerm prepared = prepare_match_term(term);
        std::istringstream words(prepared.normalized);
        std::string segment;
        while (words >> segment) {
            const bool valid = !segment.empty() &&
                std::all_of(segment.begin(), segment.end(), [](char c) {
                    const unsigned char value = static_cast<unsigned char>(c);
                    return is_ascii_alnum(value);
                });
            if (!valid || segments.size() == kMaxCompoundIdentifierSegments) {
                return;
            }
            segments.push_back(std::move(segment));
        }
    }
    if (segments.size() < 2) {
        return;
    }

    std::string compound;
    for (const std::string& segment : segments) {
        if (!compound.empty()) {
            compound.push_back('-');
        }
        compound += segment;
    }
    const std::vector<std::string> variants = automatic_term_variants(compound);
    for (std::vector<std::string>& group : groups) {
        for (const std::string& variant : variants) {
            push_unique_term(group, variant);
        }
    }
}

std::vector<std::vector<std::string>> expand_query_term_groups(const Profile& profile,
                                                               const std::vector<std::string>& terms) {
    std::vector<std::vector<std::string>> groups;
    for (const std::string& term : terms) {
        std::vector<std::string> group = automatic_term_variants(term);
        for (const AliasRule& rule : profile.aliases) {
            bool selected = false;
            const std::string canonical = to_lower(rule.term);
            if (std::find(group.begin(), group.end(), canonical) != group.end()) {
                selected = true;
            }
            for (const std::string& expansion : rule.expands) {
                const std::string lower_expansion = to_lower(expansion);
                if (std::find(group.begin(), group.end(), lower_expansion) != group.end()) {
                    selected = true;
                    break;
                }
            }
            if (!selected) {
                continue;
            }
            push_unique_term(group, rule.term);
            for (const std::string& expansion : rule.expands) {
                push_unique_term(group, expansion);
            }
        }
        groups.push_back(group);
    }
    add_compound_identifier_query_variants(terms, groups);
    return groups;
}

static QueryRequest parse_query_request(const std::vector<std::string>& args, int default_limit) {
    QueryRequest request;
    request.limit = default_limit;
    std::vector<std::string> query_parts;
    bool options_ended = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!options_ended && args[i] == "--") {
            options_ended = true;
        } else if (!options_ended && args[i] == "--limit") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--limit requires a value.");
            }
            request.limit = parse_cli_integer(args[++i], "--limit", 1);
        } else if (!options_ended && args[i] == "--per-doc") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--per-doc requires a value.");
            }
            request.per_doc_limit = parse_cli_integer(args[++i], "--per-doc", 1);
        } else if (!options_ended && args[i] == "--require-current") {
            request.require_current = true;
        } else if (!options_ended && starts_with(args[i], "--")) {
            throw_usage_error(
                "unsupported-option",
                "Query commands only accept --limit, --per-doc, and --require-current; "
                "use -- before literal query text that begins with --.");
        } else {
            query_parts.push_back(args[i]);
        }
    }
    for (const std::string& part : query_parts) {
        if (!request.query.empty()) {
            request.query.push_back(' ');
        }
        request.query += part;
    }
    request.query = trim(request.query);
    if (request.query.empty()) {
        throw_usage_error("missing-query", "A non-empty query is required.");
    }
    request.terms = parse_terms(request.query);
    request.term_groups = expand_query_term_groups(Profile{}, request.terms);
    request.prepared_term_groups = prepare_term_groups(request.term_groups);
    return request;
}

void apply_profile_query_expansion(const Profile& profile, QueryRequest& request) {
    request.term_groups = expand_query_term_groups(profile, request.terms);
    request.prepared_term_groups = prepare_term_groups(request.term_groups);
}

QueryRequest make_command_query_request(const Profile& profile,
                                        const std::vector<std::string>& args,
                                        int default_limit,
                                        int default_per_doc_limit) {
    QueryRequest request = parse_query_request(args, default_limit);
    apply_profile_query_expansion(profile, request);
    if (request.per_doc_limit == 0) {
        request.per_doc_limit = default_per_doc_limit;
    }
    return request;
}

int strict_required_group_matches(const QueryRequest& request) {
    return static_cast<int>(request.term_groups.size());
}

int relaxed_required_group_matches(const QueryRequest& request) {
    const int total = strict_required_group_matches(request);
    if (total < 3) {
        return total;
    }
    if (total >= kCompoundQueryCoverageThreshold) {
        return (total + 1) / 2;
    }
    return 2;
}

bool can_relax_query(const QueryRequest& request) {
    return relaxed_required_group_matches(request) < strict_required_group_matches(request);
}

bool should_expand_query_coverage(const QueryRequest& request, std::size_t distinct_path_count) {
    return strict_required_group_matches(request) >= kCompoundQueryCoverageThreshold &&
           distinct_path_count < 2;
}

RetrievalDiagnostics make_retrieval_diagnostics(const QueryRequest& request,
                                                int required_group_matches,
                                                bool fallback_used) {
    RetrievalDiagnostics diagnostics;
    diagnostics.fallback_used = fallback_used;
    diagnostics.required_group_matches = required_group_matches;
    diagnostics.total_term_groups = strict_required_group_matches(request);
    return diagnostics;
}

PreparedTermGroups prepare_term_groups(const std::vector<std::vector<std::string>>& term_groups);
MatchScore score_section_match(const SectionRow& section,
                               const PreparedTermGroups& term_groups,
                               int required_group_matches);
MatchScore score_fact_match(const FactRow& fact,
                            const PreparedTermGroups& term_groups,
                            int required_group_matches);
bool generated_index_available(const RuntimeOptions& options);
int generated_index_source_count(const RuntimeOptions& options);
IndexFreshness assess_index_freshness(const RuntimeOptions& options, bool require_current);
void require_current_index_or_throw(const IndexFreshness& freshness);
Json index_freshness_json(const IndexFreshness& freshness);
void emit_index_freshness_json(std::ostream& out, const IndexFreshness& freshness);
bool should_use_generated_index(const RuntimeOptions& options);
CandidateRows load_generated_query_candidates(const RuntimeOptions& options,
                                               const PreparedTermGroups& term_groups,
                                               int required_group_matches);
void for_each_generated_section_matching_terms(const RuntimeOptions& options,
                                               const CandidateRows& candidates,
                                               const PreparedTermGroups& term_groups,
                                               int required_group_matches,
                                               const std::function<void(const SectionRow&)>& handle);
void for_each_generated_fact_matching_terms(const RuntimeOptions& options,
                                            const CandidateRows& candidates,
                                            const PreparedTermGroups& term_groups,
                                            int required_group_matches,
                                            const std::function<void(const FactRow&)>& handle);

static bool search_hit_better(const SearchHit& a, const SearchHit& b) {
    if (a.score != b.score) {
        return a.score > b.score;
    }
    if (a.path != b.path) {
        return a.path < b.path;
    }
    return std::tie(a.line, a.start_line, a.end_line, a.snippet,
                    a.matched_group_indices, a.status,
                    a.authority_level, a.evidence_kind, a.verification_level,
                    a.record_type) <
           std::tie(b.line, b.start_line, b.end_line, b.snippet,
                    b.matched_group_indices, b.status,
                    b.authority_level, b.evidence_kind, b.verification_level,
                    b.record_type);
}

bool context_hit_better(const ContextHit& a, const ContextHit& b) {
    if (a.complete_query_priority != b.complete_query_priority) {
        return a.complete_query_priority;
    }
    if (a.score != b.score) {
        return a.score > b.score;
    }
    if (a.section.path != b.section.path) {
        return a.section.path < b.section.path;
    }
    return std::tie(a.section.line, a.section.start_line, a.section.end_line,
                    a.section.row_type, a.section.chunk_index, a.section.anchor,
                    a.section.level, a.section.heading, a.section.preview,
                    a.section.document_id, a.section.evidence_kind,
                    a.section.verification_level, a.section.topics,
                    a.section.indexed_text, a.section.indexed_text_omitted_bytes) <
           std::tie(b.section.line, b.section.start_line, b.section.end_line,
                    b.section.row_type, b.section.chunk_index, b.section.anchor,
                    b.section.level, b.section.heading, b.section.preview,
                    b.section.document_id, b.section.evidence_kind,
                    b.section.verification_level, b.section.topics,
                    b.section.indexed_text, b.section.indexed_text_omitted_bytes);
}

bool fact_hit_better(const FactHit& a, const FactHit& b) {
    if (a.complete_query_priority != b.complete_query_priority) {
        return a.complete_query_priority;
    }
    if (a.score != b.score) {
        return a.score > b.score;
    }
    if (a.fact.path != b.fact.path) {
        return a.fact.path < b.fact.path;
    }
    return std::tie(a.fact.line, a.fact.start_line, a.fact.end_line,
                    a.fact.id, a.fact.record_type, a.fact.status,
                    a.fact.area, a.fact.authority_level, a.fact.title, a.fact.summary,
                    a.fact.source, a.fact.evidence_kind, a.fact.verification_level,
                    a.fact.topics, a.fact.identifier_terms) <
           std::tie(b.fact.line, b.fact.start_line, b.fact.end_line,
                    b.fact.id, b.fact.record_type, b.fact.status,
                    b.fact.area, b.fact.authority_level, b.fact.title, b.fact.summary,
                    b.fact.source, b.fact.evidence_kind, b.fact.verification_level,
                    b.fact.topics, b.fact.identifier_terms);
}

void push_top_search_hit(std::vector<SearchHit>& hits, const SearchHit& hit, int limit) {
    if (limit <= 0) {
        return;
    }
    hits.push_back(hit);
    std::sort(hits.begin(), hits.end(), search_hit_better);
    if (static_cast<int>(hits.size()) > limit) {
        hits.resize(static_cast<std::size_t>(limit));
    }
}

void push_top_context_hit(std::vector<ContextHit>& hits, const ContextHit& hit, int limit) {
    if (limit <= 0) {
        return;
    }
    hits.push_back(hit);
    std::sort(hits.begin(), hits.end(), context_hit_better);
    if (static_cast<int>(hits.size()) > limit) {
        hits.resize(static_cast<std::size_t>(limit));
    }
}

void push_top_fact_hit(std::vector<FactHit>& hits, const FactHit& hit, int limit) {
    if (limit <= 0) {
        return;
    }
    hits.push_back(hit);
    std::sort(hits.begin(), hits.end(), fact_hit_better);
    if (static_cast<int>(hits.size()) > limit) {
        hits.resize(static_cast<std::size_t>(limit));
    }
}

static void add_score_component(MatchScore& match, std::string_view field, int score) {
    if (field.empty() || score <= 0) {
        return;
    }
    auto [component, inserted] = match.components.try_emplace(std::string(field), 0);
    (void)inserted;
    component->second += score;
    append_unique_string(match.matched_fields, component->first);
}

static bool explicitly_non_current_status(std::string_view status) {
    const std::string lower = to_lower(std::string(status));
    return contains_any(lower, {"draft", "proposal", "proposed", "backlog", "open",
                                "not-implemented", "not implemented"});
}

static void apply_authority_weight(MatchScore& match,
                                   std::string_view authority_level,
                                   std::string_view status) {
    int adjustment = 0;
    std::string component;
    if (authority_level == "source-of-truth") {
        adjustment = 24;
        component = "source_of_truth_boost";
    } else if (authority_level == "accepted") {
        adjustment = 16;
        component = "accepted_authority_boost";
    } else if (authority_level == "history") {
        adjustment = -20;
        component = "history_authority_penalty";
    } else if (authority_level == "review") {
        adjustment = -8;
        component = "review_authority_penalty";
    } else if (authority_level == "advisory" && explicitly_non_current_status(status)) {
        adjustment = -12;
        component = "non_current_advisory_penalty";
    }
    if (adjustment == 0) {
        return;
    }
    match.total += adjustment;
    match.components[component] += adjustment;
    if (adjustment > 0) {
        append_unique_string(match.matched_fields, "authority_level");
    }
    if (match.total < 1) {
        match.components["authority_retrieval_floor"] += 1 - match.total;
        match.total = 1;
    }
}

std::string brief_fact_group(const FactRow& fact) {
    const std::string type = to_lower(fact.record_type);
    if (type == "decision" || type == "adr" || type == "decision-index") {
        return "decisions";
    }
    if (type == "contract" || type == "schema" || type == "manifest" ||
        type == "openapi-contract" || type == "asyncapi-contract" ||
        type.find("contract") != std::string::npos || type.find("schema") != std::string::npos ||
        type.find("api") != std::string::npos) {
        return "contracts";
    }
    if (type == "runbook" || type == "deployment" || type == "operations" || type == "security" ||
        type.find("runbook") != std::string::npos) {
        return "runbooks";
    }
    return "facts";
}

bool complete_brief_match_priority(const BriefResult& result, const MatchScore& match) {
    // Preserve complete bounded records when long queries broaden. A large
    // Markdown body can contain incidental terms and keeps its existing score.
    const bool complete = result.fallback_used && result.total_term_groups >= kCompoundQueryCoverageThreshold &&
        !result.scope_fallback_used && !result.document_owner_scope_used &&
        std::find(match.matched_fields.begin(), match.matched_fields.end(), "indexed_text") ==
            match.matched_fields.end() &&
        match.matched_groups == result.total_term_groups;
    if (!complete) return false;
    // Completeness never bypasses an explicit lifecycle/authority penalty.
    for (const auto* penalty : {"history_authority_penalty", "review_authority_penalty",
                                "non_current_advisory_penalty"}) {
        if (match.components.contains(penalty)) return false;
    }
    return true;
}

void push_brief_section_match(BriefResult& result,
                              const SectionRow& section,
                              const MatchScore& match,
                              int per_doc_limit) {
    if (match.total <= 0) {
        return;
    }
    ++result.matched_sections;
    push_top_context_hit(result.section_hits_by_doc[section.path],
                         {section, match.total, match.components, match.matched_fields,
                          match.matched_group_indices, complete_brief_match_priority(result, match)},
                         per_doc_limit);
}

void push_brief_fact_match(BriefResult& result,
                           const FactRow& fact,
                           const MatchScore& match,
                           int limit,
                           int per_doc_limit) {
    if (match.total <= 0) {
        return;
    }
    ++result.matched_facts;
    const std::string group = brief_fact_group(fact);
    if (!result.seen_fact_keys.insert(fact).second) {
        ++result.deduplicated_facts;
        return;
    }
    auto push_group_hit = [&](std::vector<FactHit>& hits) {
        FactHit candidate{fact, match.total, match.components, match.matched_fields,
                          match.matched_group_indices, complete_brief_match_priority(result, match)};
        std::vector<std::size_t> same_path;
        for (std::size_t index = 0; index < hits.size(); ++index) {
            if (hits[index].fact.path == fact.path) {
                same_path.push_back(index);
            }
        }
        if (static_cast<int>(same_path.size()) >= per_doc_limit) {
            const auto worst = std::max_element(
                same_path.begin(), same_path.end(),
                [&](std::size_t a, std::size_t b) { return fact_hit_better(hits[a], hits[b]); });
            if (worst == same_path.end() || !fact_hit_better(candidate, hits[*worst])) {
                return;
            }
            hits[*worst] = std::move(candidate);
        } else {
            hits.push_back(std::move(candidate));
        }
        std::sort(hits.begin(), hits.end(), fact_hit_better);
        if (static_cast<int>(hits.size()) > limit) {
            hits.resize(static_cast<std::size_t>(limit));
        }
    };
    if (group == "decisions") {
        ++result.matched_decisions;
        push_group_hit(result.decisions);
    } else if (group == "contracts") {
        ++result.matched_contracts;
        push_group_hit(result.contracts);
    } else if (group == "runbooks") {
        ++result.matched_runbooks;
        push_group_hit(result.runbooks);
    } else {
        ++result.matched_other_facts;
        push_group_hit(result.facts);
    }
}

std::vector<ContextHit> select_context_sections(
    const std::map<std::string, std::vector<ContextHit>>& section_hits_by_doc,
    int limit,
    int per_doc_limit,
    bool prefer_distinct_paths) {
    std::vector<ContextHit> hits;
    for (const auto& [path, doc_hits] : section_hits_by_doc) {
        (void)path;
        hits.insert(hits.end(), doc_hits.begin(), doc_hits.end());
    }
    std::sort(hits.begin(), hits.end(), context_hit_better);

    std::vector<ContextHit> selected_hits;
    std::map<std::string, int> per_doc_counts;
    if (prefer_distinct_paths) {
        std::set<std::string> selected_paths;
        std::set<SectionRow> selected_section_rows;
        std::vector<bool> selected_indexes(hits.size(), false);
        for (std::size_t index = 0; index < hits.size(); ++index) {
            const ContextHit& hit = hits[index];
            if (per_doc_counts[hit.section.path] >= per_doc_limit ||
                !selected_paths.insert(hit.section.path).second ||
                !selected_section_rows.insert(hit.section).second) {
                continue;
            }
            selected_indexes[index] = true;
            selected_hits.push_back(hit);
            ++per_doc_counts[hit.section.path];
            if (static_cast<int>(selected_hits.size()) >= limit) return selected_hits;
        }
        for (std::size_t index = 0; index < hits.size(); ++index) {
            const ContextHit& hit = hits[index];
            int& count = per_doc_counts[hit.section.path];
            if (selected_indexes[index] || count >= per_doc_limit ||
                !selected_section_rows.insert(hit.section).second) {
                continue;
            }
            selected_hits.push_back(hit);
            ++count;
            if (static_cast<int>(selected_hits.size()) >= limit) return selected_hits;
        }
        return selected_hits;
    }
    for (const ContextHit& hit : hits) {
        int& count = per_doc_counts[hit.section.path];
        if (count >= per_doc_limit) {
            continue;
        }
        selected_hits.push_back(hit);
        ++count;
        if (static_cast<int>(selected_hits.size()) >= limit) {
            break;
        }
    }
    return selected_hits;
}

namespace {

enum class BriefEvidenceKind {
    Contract = 0,
    Decision = 1,
    Runbook = 2,
    Fact = 3,
    Section = 4,
    Count = 5
};

struct BriefEvidenceCandidate {
    BriefEvidenceKind kind = BriefEvidenceKind::Contract;
    std::size_t index = 0;
    int score = 0;
    std::string_view path;
    int line = 0;
    std::string_view stable_id;
    bool valid = false;
    bool complete_query_priority = false;
};

bool brief_evidence_candidate_better(const BriefEvidenceCandidate& a,
                                     const BriefEvidenceCandidate& b) {
    if (a.complete_query_priority != b.complete_query_priority) {
        return a.complete_query_priority;
    }
    if (a.score != b.score) {
        return a.score > b.score;
    }
    if (a.kind != b.kind) {
        return a.kind < b.kind;
    }
    if (a.path != b.path) {
        return a.path < b.path;
    }
    if (a.line != b.line) {
        return a.line < b.line;
    }
    if (a.stable_id != b.stable_id) {
        return a.stable_id < b.stable_id;
    }
    return a.index < b.index;
}

void consider_brief_fact_candidate(BriefEvidenceCandidate& best,
                                   BriefEvidenceKind kind,
                                   const std::vector<FactHit>& hits,
                                   std::size_t index,
                                   const std::map<std::string, int>& per_doc_counts,
                                   int per_doc_limit,
                                   const std::set<std::pair<BriefEvidenceKind, std::size_t>>* selected_records,
                                   bool require_unrepresented_path) {
    while (index < hits.size()) {
        const auto count = per_doc_counts.find(hits[index].fact.path);
        const bool at_document_limit = count != per_doc_counts.end() && count->second >= per_doc_limit;
        const bool already_selected = selected_records != nullptr &&
            selected_records->contains({kind, index});
        const bool path_already_represented = require_unrepresented_path &&
            count != per_doc_counts.end();
        if (!at_document_limit && !already_selected && !path_already_represented) break;
        ++index;
    }
    if (index >= hits.size()) {
        return;
    }
    const FactHit& hit = hits[index];
    const BriefEvidenceCandidate candidate =
        {kind, index, hit.score, hit.fact.path, hit.fact.line, hit.fact.id, true,
         hit.complete_query_priority};
    if (!best.valid || brief_evidence_candidate_better(candidate, best)) {
        best = candidate;
    }
}

void consider_brief_section_candidate(BriefEvidenceCandidate& best,
                                      const std::vector<ContextHit>& sections,
                                      std::size_t index,
                                      const std::map<std::string, int>& per_doc_counts,
                                      int per_doc_limit,
                                      const std::set<std::pair<BriefEvidenceKind, std::size_t>>* selected_records,
                                      bool require_unrepresented_path) {
    while (index < sections.size()) {
        const auto count = per_doc_counts.find(sections[index].section.path);
        const bool at_document_limit = count != per_doc_counts.end() && count->second >= per_doc_limit;
        const bool already_selected = selected_records != nullptr &&
            selected_records->contains({BriefEvidenceKind::Section, index});
        const bool path_already_represented = require_unrepresented_path &&
            count != per_doc_counts.end();
        if (!at_document_limit && !already_selected && !path_already_represented) break;
        ++index;
    }
    if (index >= sections.size()) {
        return;
    }
    const ContextHit& hit = sections[index];
    const BriefEvidenceCandidate candidate = {BriefEvidenceKind::Section,
                                               index,
                                               hit.score,
                                               hit.section.path,
                                               hit.section.line,
                                               hit.section.anchor,
                                               true,
                                               hit.complete_query_priority};
    if (!best.valid || brief_evidence_candidate_better(candidate, best)) {
        best = candidate;
    }
}

using BriefCursors = std::array<std::size_t, static_cast<std::size_t>(BriefEvidenceKind::Count)>;
using BriefBlockedGroups = std::array<bool, static_cast<std::size_t>(BriefEvidenceKind::Count)>;

void consider_brief_group_heads(BriefEvidenceCandidate& best,
                                const BriefResult& result,
                                const std::vector<ContextHit>& sections,
                                const BriefCursors& cursors,
                                const BriefBlockedGroups& blocked,
                                const std::map<std::string, int>& per_doc_counts, int per_doc_limit,
                                const std::vector<std::string>& allowed_evidence_types,
                                const std::set<std::pair<BriefEvidenceKind, std::size_t>>* selected_records = nullptr,
                                bool require_unrepresented_path = false) {
    auto allowed = [&](std::string_view type) {
        return allowed_evidence_types.empty() ||
               std::find(allowed_evidence_types.begin(), allowed_evidence_types.end(), type) !=
                   allowed_evidence_types.end();
    };
    if (!blocked[0] && allowed("contract")) {
        consider_brief_fact_candidate(best, BriefEvidenceKind::Contract, result.contracts, cursors[0], per_doc_counts, per_doc_limit,
                                      selected_records, require_unrepresented_path);
    }
    if (!blocked[1] && allowed("decision")) {
        consider_brief_fact_candidate(best, BriefEvidenceKind::Decision, result.decisions, cursors[1], per_doc_counts, per_doc_limit,
                                      selected_records, require_unrepresented_path);
    }
    if (!blocked[2] && allowed("runbook")) {
        consider_brief_fact_candidate(best, BriefEvidenceKind::Runbook, result.runbooks, cursors[2],
                                      per_doc_counts, per_doc_limit, selected_records, require_unrepresented_path);
    }
    if (!blocked[3] && allowed("fact")) {
        consider_brief_fact_candidate(best, BriefEvidenceKind::Fact, result.facts, cursors[3],
                                      per_doc_counts, per_doc_limit, selected_records, require_unrepresented_path);
    }
    if (!blocked[4] && allowed("section")) {
        consider_brief_section_candidate(best, sections, cursors[4], per_doc_counts, per_doc_limit,
                                         selected_records, require_unrepresented_path);
    }
}

void append_brief_candidate(BriefSelection& selection,
                            const BriefResult& result,
                            const std::vector<ContextHit>& sections,
                            const BriefEvidenceCandidate& candidate,
                            BriefCursors& cursors,
                            std::map<std::string, int>& per_doc_counts) {
    switch (candidate.kind) {
        case BriefEvidenceKind::Contract:
            selection.contracts.push_back(result.contracts[candidate.index]);
            selection.ordered.push_back({BriefEvidenceType::contract,
                                         selection.contracts.size() - 1});
            break;
        case BriefEvidenceKind::Decision:
            selection.decisions.push_back(result.decisions[candidate.index]);
            selection.ordered.push_back({BriefEvidenceType::decision,
                                         selection.decisions.size() - 1});
            break;
        case BriefEvidenceKind::Runbook:
            selection.runbooks.push_back(result.runbooks[candidate.index]);
            selection.ordered.push_back({BriefEvidenceType::runbook,
                                         selection.runbooks.size() - 1});
            break;
        case BriefEvidenceKind::Fact:
            selection.facts.push_back(result.facts[candidate.index]);
            selection.ordered.push_back({BriefEvidenceType::fact,
                                         selection.facts.size() - 1});
            break;
        case BriefEvidenceKind::Section:
            selection.sections.push_back(sections[candidate.index]);
            selection.ordered.push_back({BriefEvidenceType::section,
                                         selection.sections.size() - 1});
            break;
        case BriefEvidenceKind::Count:
            return;
    }
    cursors[static_cast<std::size_t>(candidate.kind)] = candidate.index + 1;
    ++per_doc_counts[std::string(candidate.path)];
}

}  // namespace

BriefSelection select_brief_evidence(const BriefResult& result,
                                     int limit,
                                     int per_doc_limit,
                                     const std::vector<std::string>& allowed_evidence_types,
                                     bool prefer_distinct_paths) {
    BriefSelection selection;
    if (limit <= 0) {
        return selection;
    }

    const std::vector<ContextHit> sections =
        select_context_sections(result.section_hits_by_doc, limit, per_doc_limit, prefer_distinct_paths);
    const auto allowed = [&](std::string_view type) {
        return allowed_evidence_types.empty() ||
               std::find(allowed_evidence_types.begin(), allowed_evidence_types.end(), type) !=
                   allowed_evidence_types.end();
    };
    selection.eligible_records = static_cast<int>(
        (allowed("section") ? sections.size() : 0) +
        (allowed("decision") ? result.decisions.size() : 0) +
        (allowed("contract") ? result.contracts.size() : 0) +
        (allowed("runbook") ? result.runbooks.size() : 0) +
        (allowed("fact") ? result.facts.size() : 0));

    BriefCursors cursors{};
    BriefBlockedGroups seeded{};
    const BriefBlockedGroups no_blocked_groups{};
    std::set<std::pair<BriefEvidenceKind, std::size_t>> selected_records;
    std::map<std::string, int> per_doc_counts;
    int returned_records = 0;
    while (returned_records < limit) {
        BriefEvidenceCandidate best;
        consider_brief_group_heads(best, result, sections, cursors, seeded, per_doc_counts,
                                   per_doc_limit, allowed_evidence_types);
        if (!best.valid) {
            break;
        }
        append_brief_candidate(selection, result, sections, best, cursors, per_doc_counts);
        selected_records.insert({best.kind, best.index});
        seeded[static_cast<std::size_t>(best.kind)] = true;
        ++returned_records;
    }

    // A query result is a bounded evidence list, but its ranked-path view should not
    // lose a unique document merely because an earlier document has spare evidence
    // slots. Keep the existing evidence-kind seeds, then give each new path its
    // strongest eligible row before returning to the normal global score order.
    if (prefer_distinct_paths) {
        const BriefCursors seed_cursors = cursors;
        BriefCursors diversity_cursors = seed_cursors;
        while (returned_records < limit) {
            BriefEvidenceCandidate best;
            consider_brief_group_heads(best, result, sections, diversity_cursors, no_blocked_groups,
                                       per_doc_counts, per_doc_limit, allowed_evidence_types,
                                       &selected_records, true);
            if (!best.valid) break;
            append_brief_candidate(selection, result, sections, best, diversity_cursors, per_doc_counts);
            selected_records.insert({best.kind, best.index});
            ++returned_records;
        }
        cursors = seed_cursors;
    }

    while (returned_records < limit) {
        BriefEvidenceCandidate best;
        consider_brief_group_heads(best, result, sections, cursors, no_blocked_groups,
                                   per_doc_counts, per_doc_limit, allowed_evidence_types,
                                   &selected_records, false);
        if (!best.valid) {
            break;
        }
        append_brief_candidate(selection, result, sections, best, cursors, per_doc_counts);
        selected_records.insert({best.kind, best.index});
        ++returned_records;
    }
    selection.budgeted_out_records = selection.eligible_records - returned_records;
    return selection;
}

void add_related_document(std::vector<std::string>& related_documents,
                          std::set<std::string>& seen_documents,
                          const std::string& path) {
    if (!path.empty() && seen_documents.insert(path).second) {
        related_documents.push_back(path);
    }
}

Json section_hit_json(const ContextHit& hit) {
    const SectionRow& section = hit.section;
    return {
        {"path", section.path},
        {"line", section.line},
        {"status", section.status},
        {"authority_level", section.authority_level},
        {"evidence_kind", section.evidence_kind},
        {"verification_level", section.verification_level},
        {"start_line", section.start_line},
        {"end_line", section.end_line},
        {"level", section.level},
        {"row_type", section.row_type},
        {"chunk_index", section.chunk_index},
        {"heading", section.heading},
        {"score", hit.score},
        {"score_components", hit.score_components},
        {"matched_fields", hit.matched_fields},
        {"matched_group_indices", hit.matched_group_indices},
        {"topics", section.topics},
        {"preview", section.preview},
        {"indexed_text", section.indexed_text},
        {"indexed_text_bytes", section.indexed_text.size()},
        {"indexed_text_omitted_bytes", section.indexed_text_omitted_bytes}
    };
}

Json fact_hit_json(const FactHit& hit) {
    const FactRow& fact = hit.fact;
    Json output = {
        {"id", fact.id},
        {"record_type", fact.record_type},
        {"path", fact.path},
        {"line", fact.line},
        {"start_line", fact.start_line},
        {"end_line", fact.end_line},
        {"score", hit.score},
        {"score_components", hit.score_components},
        {"matched_fields", hit.matched_fields},
        {"matched_group_indices", hit.matched_group_indices},
        {"status", fact.status},
        {"area", fact.area},
        {"authority_level", fact.authority_level},
        {"evidence_kind", fact.evidence_kind},
        {"verification_level", fact.verification_level},
        {"title", fact.title},
        {"summary", fact.summary},
        {"source", fact.source},
        {"topics", fact.topics}
    };
    if (!fact.range_origin.empty()) output["range_origin"] = fact.range_origin;
    return output;
}

void emit_section_hit_json(std::ostream& out, const ContextHit& hit) {
    out << section_hit_json(hit).dump();
}

void emit_fact_hit_json(std::ostream& out, const FactHit& hit) {
    out << fact_hit_json(hit).dump();
}

void emit_section_hit_array(std::ostream& out, const std::vector<ContextHit>& hits, const std::string& indent) {
    for (std::size_t i = 0; i < hits.size(); ++i) {
        out << indent;
        emit_section_hit_json(out, hits[i]);
        if (i + 1 < hits.size()) {
            out << ",";
        }
        out << "\n";
    }
}

void emit_fact_hit_array(std::ostream& out, const std::vector<FactHit>& hits, const std::string& indent) {
    for (std::size_t i = 0; i < hits.size(); ++i) {
        out << indent;
        emit_fact_hit_json(out, hits[i]);
        if (i + 1 < hits.size()) {
            out << ",";
        }
        out << "\n";
    }
}

static SearchIndexResult collect_search_from_index(const RuntimeOptions& options,
                                            const QueryRequest& request,
                                            int required_group_matches,
                                            bool fallback_used) {
    SearchIndexResult result;
    result.diagnostics = make_retrieval_diagnostics(request, required_group_matches, fallback_used);
    const CandidateRows candidates =
        load_generated_query_candidates(options, request.prepared_term_groups, required_group_matches);
    result.diagnostics.candidate_rows = candidates.discovered_row_count;
    result.diagnostics.candidate_index_used = candidates.from_terms_index;
    for_each_generated_section_matching_terms(options, candidates, request.prepared_term_groups,
                                              required_group_matches,
                                              [&](const SectionRow& section) {
        MatchScore match = score_section_match(section, request.prepared_term_groups, required_group_matches);
        if (match.total <= 0) {
            return;
        }
        ++result.matched;
        ++result.matched_sections;
        std::string snippet = section.heading;
        if (!section.preview.empty()) {
            snippet += " " + section.preview;
        }
        push_top_search_hit(result.hits,
                            {section.path, truncate_text(compact_whitespace(snippet), 220), section.line,
                             section.start_line, section.end_line, match.total,
                             match.matched_group_indices, section.status,
                             section.authority_level, section.evidence_kind,
                             section.verification_level, section.row_type},
                            request.limit);
    });
    for_each_generated_fact_matching_terms(options, candidates, request.prepared_term_groups,
                                           required_group_matches,
                                           [&](const FactRow& fact) {
        MatchScore match = score_fact_match(fact, request.prepared_term_groups, required_group_matches);
        if (match.total <= 0) {
            return;
        }
        ++result.matched;
        ++result.matched_facts;
        std::string snippet = "[" + fact.record_type + "] " + fact.title;
        if (!fact.summary.empty()) {
            snippet += " " + fact.summary;
        }
        push_top_search_hit(result.hits,
                            {fact.path, truncate_text(compact_whitespace(snippet), 220), fact.line,
                             fact.start_line, fact.end_line, match.total,
                             match.matched_group_indices, fact.status,
                             fact.authority_level, fact.evidence_kind,
                             fact.verification_level, fact.record_type},
                            request.limit);
    });
    std::sort(result.hits.begin(), result.hits.end(), search_hit_better);
    return result;
}

SearchIndexResult collect_search_from_index_with_fallback(const RuntimeOptions& options,
                                                          const QueryRequest& request) {
    SearchIndexResult result =
        collect_search_from_index(options, request, strict_required_group_matches(request), false);
    std::set<std::string> result_paths;
    for (const SearchHit& hit : result.hits) {
        result_paths.insert(hit.path);
    }
    if ((!result.hits.empty() && !should_expand_query_coverage(request, result_paths.size())) ||
        !can_relax_query(request)) {
        return result;
    }
    return collect_search_from_index(options, request, relaxed_required_group_matches(request), true);
}

static Json make_search_result(const RuntimeOptions& options,
                               const QueryRequest& request,
                               const std::string& index_mode,
                               const IndexFreshness& freshness,
                               const SearchIndexResult& result) {
    const int returned = std::min(request.limit, static_cast<int>(result.hits.size()));
    QueryCoverage coverage = make_query_coverage(request);
    for (int index = 0; index < returned; ++index) {
        const SearchHit& hit = result.hits[static_cast<std::size_t>(index)];
        add_query_coverage_evidence(coverage, hit.path, hit.matched_group_indices);
    }
    Json output = make_result_document("shin.search.v1", "search", true);
    output["profile"] = options.profile.id;
    output["index_mode"] = index_mode;
    output["index_freshness"] = index_freshness_json(freshness);
    output["snapshot_id"] = freshness.snapshot_id.empty() ? Json(nullptr) : Json(freshness.snapshot_id);
    output["query"] = request.query;
    output["retrieval_mode"] = result.diagnostics.fallback_used ? "relaxed" : "strict";
    output["query_diagnostics"] = {
        {"fallback_used", result.diagnostics.fallback_used},
        {"candidate_rows", result.diagnostics.candidate_rows},
        {"candidate_index_used", result.diagnostics.candidate_index_used},
        {"required_group_matches", result.diagnostics.required_group_matches},
        {"total_term_groups", result.diagnostics.total_term_groups},
        {"matching_mode", kMatchingMode},
        {"topic_match_policy", "booster-only"}
    };
    output["query_coverage"] = query_coverage_json(coverage);
    output["limit"] = request.limit;
    output["matched"] = result.matched;
    output["returned"] = returned;
    output["hits"] = Json::array();
    for (int i = 0; i < returned; ++i) {
        const SearchHit& hit = result.hits[static_cast<std::size_t>(i)];
        output["hits"].push_back({
            {"path", hit.path},
            {"line", hit.line},
            {"start_line", hit.start_line},
            {"end_line", hit.end_line},
            {"score", hit.score},
            {"matched_group_indices", hit.matched_group_indices},
            {"status", hit.status},
            {"authority_level", hit.authority_level},
            {"evidence_kind", hit.evidence_kind},
            {"verification_level", hit.verification_level},
            {"record_type", hit.record_type},
            {"snippet", hit.snippet}
        });
    }
    return output;
}

static int emit_search_result(const RuntimeOptions& options,
                              const QueryRequest& request,
                              const std::string& index_mode,
                              const IndexFreshness& freshness,
                              const SearchIndexResult& result) {
    emit_json_document(std::cout, make_search_result(options, request, index_mode, freshness, result));
    return 0;
}

Json RetrievalSession::search(const QueryRequest& request) const {
    if (request.require_current) require_current_index_or_throw(freshness_);
    return make_search_result(options_, request, "generated-index", freshness_,
                              collect_search_from_index_with_fallback(options_, request));
}

static int run_search_from_index(const RuntimeOptions& options, const QueryRequest& request) {
    RetrievalSession session(options, request.require_current);
    emit_json_document(std::cout, session.search(request));
    return 0;
}

static SearchIndexResult collect_search_from_sources(const RuntimeOptions& options,
                                                      const QueryRequest& request,
                                                      const SourceScanCorpus& corpus,
                                                      int required_group_matches,
                                                      bool fallback_used) {
    (void)options;
    SearchIndexResult result;
    result.diagnostics = make_retrieval_diagnostics(request, required_group_matches, fallback_used);
    for (const SourceDoc& doc : corpus.documents) {
        const std::string authority_level = infer_authority_level(
            doc.doc_type, "section", doc.path, doc.status);
        for (std::size_t i = 0; i < doc.lines.size(); ++i) {
            const std::string& raw_line = doc.lines[i];
            std::string compact = compact_whitespace(raw_line);
            if (compact.empty()) {
                continue;
            }
            std::vector<int> matched_group_indices;
            int score = 0;
            for (std::size_t group_index = 0; group_index < request.term_groups.size(); ++group_index) {
                const std::vector<std::string>& group = request.term_groups[group_index];
                bool group_matched = false;
                int group_score = 0;
                for (const std::string& term : group) {
                    if (contains_ascii_case_insensitive(raw_line, term)) {
                        group_score = std::max(group_score, 10);
                    }
                    if (contains_ascii_case_insensitive(doc.path, term)) {
                        group_score = std::max(group_score, 20);
                    }
                    if (group_score > 0) {
                        group_matched = true;
                    }
                }
                if (group_matched) {
                    matched_group_indices.push_back(static_cast<int>(group_index));
                    score += group_score;
                }
            }
            if (static_cast<int>(matched_group_indices.size()) < required_group_matches) {
                continue;
            }
            if (starts_with(trim(raw_line), "#")) {
                score += 5;
            }
            ++result.matched;
            ++result.matched_sections;
            push_top_search_hit(
                result.hits,
                {doc.path, truncate_text(compact, 220), static_cast<int>(i + 1),
                 static_cast<int>(i + 1), static_cast<int>(i + 1), score,
                 matched_group_indices, doc.status, authority_level, doc.evidence_kind,
                 doc.verification_level, doc.doc_type},
                request.limit);
        }
    }
    for (const FactRow& fact : corpus.source_facts.rows) {
        MatchScore match = score_fact_match(fact, request.prepared_term_groups, required_group_matches);
        if (match.total <= 0) {
            continue;
        }
        ++result.matched;
        ++result.matched_facts;
        std::string snippet = "[" + fact.record_type + "] " + fact.title;
        if (!fact.summary.empty()) {
            snippet += " " + fact.summary;
        }
        push_top_search_hit(result.hits,
                            {fact.path, truncate_text(compact_whitespace(snippet), 220), fact.line,
                             fact.start_line, fact.end_line, match.total,
                             match.matched_group_indices, fact.status, fact.authority_level,
                             fact.evidence_kind, fact.verification_level, fact.record_type},
                            request.limit);
    }
    std::sort(result.hits.begin(), result.hits.end(), search_hit_better);
    return result;
}

static SearchIndexResult collect_search_from_sources_with_fallback(
    const RuntimeOptions& options,
    const QueryRequest& request,
    const SourceScanCorpus& corpus) {
    SearchIndexResult result = collect_search_from_sources(
        options, request, corpus, strict_required_group_matches(request), false);
    std::set<std::string> result_paths;
    for (const SearchHit& hit : result.hits) {
        result_paths.insert(hit.path);
    }
    if ((!result.hits.empty() && !should_expand_query_coverage(request, result_paths.size())) ||
        !can_relax_query(request)) {
        return result;
    }
    return collect_search_from_sources(
        options, request, corpus, relaxed_required_group_matches(request), true);
}

static int run_search_from_sources(const RuntimeOptions& options, const QueryRequest& request) {
    const SourceScanCorpus corpus = build_source_scan_corpus(options.profile, options.root);
    const IndexFreshness freshness = source_scan_freshness(corpus);
    if (request.require_current) {
        require_complete_source_scan_or_throw(freshness);
    }
    SearchIndexResult result = collect_search_from_sources_with_fallback(options, request, corpus);
    return emit_search_result(options, request, "source-scan", freshness, result);
}

int run_search(const RuntimeOptions& options, const std::vector<std::string>& args) {
    QueryRequest request = make_command_query_request(options.profile, args, 10, 0);
    if (should_use_generated_index(options)) {
        return run_search_from_index(options, request);
    }
    return run_search_from_sources(options, request);
}

std::string normalize_match_text(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    bool previous_space = true;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char raw = text[index];
        const unsigned char c = static_cast<unsigned char>(raw);
        if (identifier_word_boundary(text, index) && !previous_space) {
            out.push_back(' ');
            previous_space = true;
        }
        if (is_ascii_alnum(c)) {
            out.push_back(static_cast<char>(ascii_lower(c)));
            previous_space = false;
        } else if (!previous_space) {
            out.push_back(' ');
            previous_space = true;
        }
    }
    // The loop emits only ASCII words and single spaces, with no leading space.
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static std::string compact_match_text(std::string_view text) {
    std::string out;
    for (char raw : text) {
        if (!is_ascii_space(static_cast<unsigned char>(raw))) {
            out.push_back(raw);
        }
    }
    return out;
}

PreparedMatchTerm prepare_match_term(std::string_view term) {
    PreparedMatchTerm prepared;
    prepared.raw = trim(to_lower(std::string(term)));
    prepared.normalized = normalize_match_text(term);
    prepared.compact = compact_match_text(prepared.normalized);
    return prepared;
}

PreparedTermGroups prepare_term_groups(const std::vector<std::vector<std::string>>& term_groups) {
    PreparedTermGroups prepared_groups;
    prepared_groups.reserve(term_groups.size());
    for (const std::vector<std::string>& group : term_groups) {
        PreparedTermGroup prepared_group;
        prepared_group.reserve(group.size());
        for (const std::string& term : group) {
            PreparedMatchTerm prepared = prepare_match_term(term);
            if (prepared.normalized.find(' ') != std::string::npos && prepared.raw.find('/') == std::string::npos) {
                std::istringstream words(prepared.normalized);
                std::string previous;
                for (std::string word; words >> word;) {
                    if (!previous.empty()) {
                        const auto pair = previous + " " + word;
                        if (index_term_has_signal(pair)) prepared.phrase_keys.push_back(pair);
                    }
                    previous.swap(word);
                }
            }
            if (!prepared.raw.empty()) {
                prepared_group.push_back(std::move(prepared));
            }
        }
        prepared_groups.push_back(std::move(prepared_group));
    }
    return prepared_groups;
}

static int score_field_group(FieldMatchInput& field, const PreparedTermGroup& group,
                             bool single_query_group) {
    bool prefix = false;
    int best_exact_score = 0;
    for (const auto& term : group) {
        const auto kind = field_match_kind(field, term);
        if (kind == LexicalMatchKind::exact) {
            if (field.name != "identifier_references") return field.score;
            if (!field.normalized_ready) {
                field.normalized_text = normalize_match_text(field.text);
                field.normalized_ready = true;
            }
            const bool complete_reference = field.normalized_text == term.normalized ||
                (field.text.size() == term.raw.size() &&
                 contains_ascii_case_insensitive(field.text, term.raw));
            const bool compound_reference = field.normalized_text.find(' ') != std::string::npos;
            if (complete_reference && (single_query_group || compound_reference)) return field.score;
            // Bare words in a multi-group query are body evidence, even when a local
            // identifier has the same spelling. Keep targeted compound lookups distinct.
            best_exact_score = std::max(best_exact_score, 16);
        }
        prefix = prefix || kind == LexicalMatchKind::prefix;
    }
    return best_exact_score > 0 ? best_exact_score : (prefix ? 1 : 0);
}


static MatchScore score_fields(
#if defined(SHIN_EXPERIMENT_FIELD_PREPARATION) || defined(SHIN_EXPERIMENT_ROW_BATCH)
                               std::vector<FieldMatchInput>& fields,
#else
                               std::vector<FieldMatchInput> fields,
#endif
                               const PreparedTermGroups& term_groups,
                               int required_group_matches) {
    // Visit the highest score bounds first so strong identifier evidence can
    // make a long summary irrelevant. Keep original positions for effective
    // ties: prefixes score one even when their nominal bounds differ.
    std::vector<std::size_t> order;
    order.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](auto left, auto right) {
        return fields[left].score != fields[right].score ? fields[left].score > fields[right].score : left < right;
    });
    MatchScore match;
    for (std::size_t group_index = 0; group_index < term_groups.size(); ++group_index) {
        const PreparedTermGroup& group = term_groups[group_index];
        FieldMatchInput* best_primary = nullptr;
        int best_primary_score = 0;
        std::size_t best_primary_index = 0;
        FieldMatchInput* best_booster = nullptr;
        int best_booster_score = 0;
        std::size_t best_booster_index = 0;
        for (const auto index : order) {
            FieldMatchInput& field = fields[index];
            const int best_score = field.primary ? best_primary_score : best_booster_score;
            const auto best_index = field.primary ? best_primary_index : best_booster_index;
            // A bound equal to the best effective score may still win an
            // original-order tie. Primary and booster winners stay independent.
            if (field.score < best_score || (field.score == best_score && index >= best_index)) continue;
            const int field_score = score_field_group(field, group, term_groups.size() == 1);
            if (field_score <= 0) {
                continue;
            }
            if (field.primary) {
                if (field_score > best_primary_score ||
                    (field_score == best_primary_score && index < best_primary_index)) {
                    best_primary_score = field_score;
                    best_primary = &field;
                    best_primary_index = index;
                }
            } else if (field_score > best_booster_score ||
                    (field_score == best_booster_score && index < best_booster_index)) {
                best_booster_score = field_score;
                best_booster = &field;
                best_booster_index = index;
            }
        }
        if (best_primary != nullptr) {
            ++match.matched_groups;
            match.matched_group_indices.push_back(static_cast<int>(group_index));
            match.total += best_primary_score;
            add_score_component(match, best_primary->name, best_primary_score);
            if (best_primary_score == 1) append_unique_string(match.matched_fields, "prefix_completion");
            if (best_booster != nullptr) {
                match.total += best_booster_score;
                add_score_component(match, best_booster->name, best_booster_score);
            }
        }
    }
    const int required =
        required_group_matches > 0 ? required_group_matches : static_cast<int>(term_groups.size());
    if (match.matched_groups < required) {
        match.total = 0;
        match.components.clear();
        match.matched_fields.clear();
    }
    return match;
}

MatchScore score_section_match(const SectionRow& section,
                               const PreparedTermGroups& term_groups,
                               int required_group_matches) {
#ifdef SHIN_EXPERIMENT_AGGREGATE_RECOVERY
    experiment::record_aggregate_score();
#endif
#ifdef SHIN_EXPERIMENT_EVAL_BATCH
    experiment::record_route_score();
#endif
#ifdef SHIN_EXPERIMENT_FIELD_PREPARATION
    auto lease = experiment::prepared_section_fields(section);
    auto fresh = lease.get() ? std::vector<FieldMatchInput>{} : section_match_fields(section);
    MatchScore match = score_fields(lease.get() ? *lease.get() : fresh, term_groups, required_group_matches);
#elif defined(SHIN_EXPERIMENT_ROW_BATCH)
    auto* prepared = experiment::prepared_batch_fields(section);
    auto fresh = prepared ? std::vector<FieldMatchInput>{} : section_match_fields(section);
    MatchScore match = score_fields(prepared ? *prepared : fresh, term_groups, required_group_matches);
#else
    MatchScore match = score_fields(section_match_fields(section), term_groups, required_group_matches);
#endif
    if (match.total <= 0) {
        return match;
    }
    if (section.row_type == "chunk") {
        match.total += 1;
        add_score_component(match, "row_type_boost", 1);
    } else if (section.level == 1) {
        match.total += 6;
        add_score_component(match, "heading_level", 6);
    } else if (section.level == 2) {
        match.total += 3;
        add_score_component(match, "heading_level", 3);
    }
    apply_authority_weight(match, section.authority_level, section.status);
    return match;
}

MatchScore score_fact_match(const FactRow& fact,
                            const PreparedTermGroups& term_groups,
                            int required_group_matches) {
#ifdef SHIN_EXPERIMENT_AGGREGATE_RECOVERY
    experiment::record_aggregate_score();
#endif
#ifdef SHIN_EXPERIMENT_EVAL_BATCH
    experiment::record_route_score();
#endif
#ifdef SHIN_EXPERIMENT_FIELD_PREPARATION
    auto lease = experiment::prepared_fact_fields(fact);
    auto fresh = lease.get() ? std::vector<FieldMatchInput>{} : fact_match_fields(fact);
    MatchScore match = score_fields(lease.get() ? *lease.get() : fresh, term_groups, required_group_matches);
#elif defined(SHIN_EXPERIMENT_ROW_BATCH)
    auto* prepared = experiment::prepared_batch_fields(fact);
    auto fresh = prepared ? std::vector<FieldMatchInput>{} : fact_match_fields(fact);
    MatchScore match = score_fields(prepared ? *prepared : fresh, term_groups, required_group_matches);
#else
    MatchScore match = score_fields(fact_match_fields(fact), term_groups, required_group_matches);
#endif
    if (match.total <= 0) {
        return match;
    }
    const std::string group = brief_fact_group(fact);
    if (group == "contracts") {
        match.total += 8;
        add_score_component(match, "record_group", 8);
    } else if (group == "decisions" || group == "runbooks") {
        match.total += 5;
        add_score_component(match, "record_group", 5);
    }
    if (starts_with(fact.source, "source-fact:")) {
        match.total -= 3;
        match.components["source_fact_penalty"] -= 3;
    }
    apply_authority_weight(match, fact.authority_level, fact.status);
    return match;
}

} // namespace shin::context
