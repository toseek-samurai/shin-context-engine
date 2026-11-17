#include "context_internal.h"

#include <iostream>

namespace shin::context {
namespace {

void expect_score(const FactRow& row, const std::vector<std::string>& terms,
                  int total, const std::map<std::string, int>& components,
                  const std::vector<std::string>& fields, const std::vector<int>& groups, bool prefix = false) {
    std::vector<std::vector<std::string>> aliases;
    aliases.reserve(terms.size());
    for (const auto& term : terms) aliases.push_back({term});
    auto prepared = prepare_term_groups(aliases);
    for (auto& group : prepared) for (auto& term : group) term.allow_prefix = prefix;
    const auto score = score_fact_match(row, prepared, static_cast<int>(terms.size()));
    if (score.total != total || score.components != components || score.matched_fields != fields ||
        score.matched_group_indices != groups || score.matched_groups != static_cast<int>(groups.size())) {
        throw std::runtime_error("scoring changed expected total, best field, tie, or group provenance");
    }
}

void check_best_field_and_booster() {
    FactRow row;
    row.title = row.summary = row.id = row.path = "needle";
    row.identifier_terms = row.topics = {"needle"};
    expect_score(row, {"needle"}, 39, {{"title", 36}, {"topics", 3}}, {"title", "topics"}, {0});
    row.title.clear();
    expect_score(row, {"needle"}, 35, {{"identifier_references", 32}, {"topics", 3}},
                 {"identifier_references", "topics"}, {0});
    row.identifier_terms.clear();
    expect_score(row, {"needle"}, 25, {{"id", 22}, {"topics", 3}}, {"id", "topics"}, {0});
}

void check_tie_provenance() {
    FactRow row;
    row.summary = row.record_type = "needle";
    expect_score(row, {"needle"}, 20, {{"summary", 20}}, {"summary"}, {0});
    row = FactRow{};
    row.status = row.area = "needle";
    expect_score(row, {"needle"}, 10, {{"status", 10}}, {"status"}, {0});
}

void check_effective_score_order() {
    FactRow row;
    // Prefix scores tie at one despite different nominal bounds (20/22/32).
    // Original summary precedence must survive any evaluation reordering.
    row.summary = row.id = "needleSuffix";
    row.identifier_terms = {"needleSuffix"};
    expect_score(row, {"need"}, 1, {{"summary", 1}}, {"summary", "prefix_completion"}, {0}, true);
    row.summary.clear();
    expect_score(row, {"need"}, 1, {{"id", 1}}, {"id", "prefix_completion"}, {0}, true);
    row.id.clear();
    row.path = "needleSuffix";
    expect_score(row, {"need"}, 1, {{"path", 1}}, {"path", "prefix_completion"}, {0}, true);
    // A high nominal identifier bound can produce weaker body evidence (16).
    // It must not hide the later-evaluated summary's exact score (20).
    row = FactRow{};
    row.summary = "alpha beta";
    row.identifier_terms = {"alpha", "beta"};
    expect_score(row, {"alpha", "beta"}, 40, {{"summary", 40}}, {"summary"}, {0, 1});
    row.summary.clear();
    row.path = "alpha beta";
    expect_score(row, {"alpha", "beta"}, 32, {{"identifier_references", 32}},
                 {"identifier_references"}, {0, 1});
}

void check_independent_groups() {
    FactRow row;
    row.title = "alpha";
    row.summary = "alpha beta";
    expect_score(row, {"alpha", "beta"}, 56, {{"title", 36}, {"summary", 20}},
                 {"title", "summary"}, {0, 1});
    row.summary.clear();
    row.topics = {"beta"};
    expect_score(row, {"alpha", "beta"}, 0, {}, {}, {0});
    row.title.clear();
    expect_score(row, {"beta"}, 0, {}, {}, {});
}

void check_normalization_bytes() {
    const auto reference = [](std::string_view text) {
        std::string out;
        bool previous_space = true;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (identifier_word_boundary(text, i) && !previous_space) {
                out.push_back(' '); previous_space = true;
            }
            if (is_ascii_alnum(c)) {
                out.push_back(static_cast<char>(ascii_lower(c))); previous_space = false;
            } else if (!previous_space) {
                out.push_back(' '); previous_space = true;
            }
        }
        return compact_whitespace(out);
    };
    // Preserve every ASCII byte pair, including embedded NULs and identifier boundaries.
    // Valid UTF-8 and malformed sequences have separate Unicode regression coverage.
    for (unsigned a = 0; a < 128; ++a) for (unsigned b = 0; b < 128; ++b) {
        const std::string input = "  HTTPServer7" + std::string{static_cast<char>(a), static_cast<char>(b)} + "FooBAR_ ";
        if (normalize_match_text(input) != reference(input))
            throw std::runtime_error("normalization changed byte semantics");
    }
}

void check_normalization_and_source_penalty() {
    FactRow row;
    row.title = "AlphaMarker";
    row.summary = "alpha marker";
    row.source = "source-fact:cpp";
    expect_score(row, {"alpha-marker"}, 33, {{"title", 36}, {"source_fact_penalty", -3}},
                 {"title"}, {0});
}

void check_row_vocabulary_reuse() {
    FactRow row;
    row.title = "AlphaMarker";
    row.summary = "alpha beta café состояние owner=apps/admin/AtlasPanel";
    row.identifier_terms = {"alpha", "beta"};
    row.source = "source-fact:cpp";
    auto fields = fact_match_fields(row);
    SectionRow section;
    section.heading = row.title;
    section.indexed_text = row.summary;
    section.level = 2;
    auto section_fields = section_match_fields(section);
    const auto equal = [](const MatchScore& left, const MatchScore& right) {
        return left.total == right.total && left.components == right.components &&
            left.matched_fields == right.matched_fields &&
            left.matched_group_indices == right.matched_group_indices &&
            left.matched_groups == right.matched_groups;
    };
    // A one-group scope and a multi-group query weight identifier references
    // differently; warm lexical fields must preserve both scores and provenance.
    for (const auto& terms : std::vector<std::vector<std::string>>{
             {"AtlasPanel"}, {"alpha", "beta"}, {"café", "состояние"},
             {"absent"}, {"AlphaMarker"}, {"alpha beta"}}) {
        std::vector<std::vector<std::string>> aliases;
        for (const auto& term : terms) aliases.push_back({term});
        const auto prepared = prepare_term_groups(aliases);
        if (!equal(score_fact_match(row, prepared, 1), score_fact_match(row, prepared, 1, &fields)) ||
            !equal(score_section_match(section, prepared, 1),
                   score_section_match(section, prepared, 1, &section_fields)))
            throw std::runtime_error("reused row fields changed scoring or group provenance");
    }
}

void check_section_score_bounds() {
    SectionRow row;
    row.document_id = "guide-needle";
    row.path = "docs/guide-needle.md";
    row.heading = "Needle café состояние";
    row.indexed_text = "A needle phrase describes the operating state.";
    row.preview = row.indexed_text;
    row.topics = {"needle", "состояние"};
    auto groups = prepare_term_groups({{"needle", "need"}, {"café"}, {"состояние"}, {"needle phrase"}, {"absent"}});
    for (bool prefix : {false, true}) {
        for (auto& group : groups) for (auto& term : group) term.allow_prefix = prefix;
        for (const auto* authority : {"source-of-truth", "accepted", "advisory", "review", "history"})
            for (const auto* status : {"active", "accepted", "superseded", "draft"})
                for (const auto* type : {"section", "chunk"})
                    for (int level : {1, 2, 4}) {
                        row.authority_level = authority; row.status = status;
                        row.row_type = type; row.level = level;
                        const auto score = score_section_match(row, groups, 1);
                        std::uint64_t mask = 0;
                        for (const int group : score.matched_group_indices) mask |= std::uint64_t{1} << group;
                        if (section_match_upper_bound(row, groups, mask) < score.total ||
                            section_match_upper_bound(row, groups, 31) < score.total)
                            throw std::runtime_error("metadata/body bound excludes a real section score");
                        for (auto count = static_cast<std::size_t>(score.matched_groups); count <= 64; ++count)
                            if (section_match_upper_bound(row, count) < score.total)
                                throw std::runtime_error("section score bound excludes real evidence");
                    }
    }
    if (section_match_upper_bound(row, 0) != 0)
        throw std::runtime_error("empty posting coverage has a positive score bound");
}

}  // namespace
}  // namespace shin::context

int main() {
    try {
        shin::context::check_best_field_and_booster();
        shin::context::check_tie_provenance();
        shin::context::check_effective_score_order();
        shin::context::check_independent_groups();
        shin::context::check_normalization_and_source_penalty();
        shin::context::check_normalization_bytes();
        shin::context::check_row_vocabulary_reuse();
        shin::context::check_section_score_bounds();
        std::cout << "retrieval scoring: 8 checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
