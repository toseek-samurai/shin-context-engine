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
        std::cout << "retrieval scoring: 6 checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
