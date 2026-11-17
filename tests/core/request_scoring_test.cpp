#include "context_internal.h"
#include "request_scoring.h"

#include <future>

using namespace shin::context;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool equal(const MatchScore& a, const MatchScore& b) {
    return a.total == b.total && a.components == b.components && a.matched_fields == b.matched_fields &&
        a.matched_groups == b.matched_groups && a.matched_group_indices == b.matched_group_indices;
}
void threshold_and_row_identity() {
    auto groups = prepare_term_groups({{"alpha"}, {"café"}, {"ручная проверка"}, {"absent"}});
    SectionRow section;
    section.path = "docs/guide.md"; section.heading = "Alpha";
    section.indexed_text = "café: ручная проверка"; section.level = 2;
    FactRow fact;
    fact.path = "src/policy.cpp"; fact.id = "stable"; fact.title = "Alpha";
    fact.summary = section.indexed_text; fact.source = "source-fact:cpp";
    RequestScoring work(groups);
    for (int threshold : {4, 3, 1, 0, -1, 2, 1}) {
        require(equal(score_section_match(section, groups, threshold),
                      score_section_match(section, groups, threshold, nullptr, &work)), "section threshold/provenance changed");
        require(equal(score_fact_match(fact, groups, threshold),
                      score_fact_match(fact, groups, threshold, nullptr, &work)), "fact threshold/provenance changed");
    }
    require(work.metrics()["score_computations"] == 2 && work.metrics()["score_reuses"] == 12,
            "thresholds did not share one complete row score");
    for (int changed = 0; changed < 5; ++changed) {
        auto value = fact;
        if (changed == 0) value.summary = "absent";
        if (changed == 1) value.authority_level = "history";
        if (changed == 2) value.status = "draft";
        if (changed == 3) value.evidence_kind = "test-source";
        if (changed == 4) value.start_line = 17;
        require(equal(score_fact_match(value, groups, 2), score_fact_match(value, groups, 2, nullptr, &work)),
                "same identifier hid changed row data");
    }
    require(work.metrics()["score_computations"] == 7, "row keys omitted source or provenance fields");
    // Prefix-only source evidence can become negative after its source penalty.
    auto prefixes = prepare_term_groups({{"alph"}, {"missing"}});
    prefixes[0][0].allow_prefix = true;
    fact = FactRow{}; fact.title = "AlphaSuffix"; fact.source = "source-fact:cpp";
    RequestScoring prefix_work(prefixes);
    for (int threshold : {2, 1, 2})
        require(equal(score_fact_match(fact, prefixes, threshold),
                      score_fact_match(fact, prefixes, threshold, nullptr, &prefix_work)), "prefix penalty or rejected score changed");
}
void aliases_and_prepared_fields() {
    auto groups = prepare_term_groups({{"alpha"}});
    SectionRow row; row.heading = "AlphaSuffix";
    RequestScoring work(groups);
    (void)score_section_match(row, groups, 1, nullptr, &work);
    auto changed = groups;
    changed[0][0].allow_prefix = true;
    require(equal(score_section_match(row, changed, 1), score_section_match(row, changed, 1, nullptr, &work)),
            "changed prefix policy reused a score");
    changed = prepare_term_groups({{"unrelated", "AlphaSuffix"}});
    require(equal(score_section_match(row, changed, 1), score_section_match(row, changed, 1, nullptr, &work)),
            "changed aliases reused a score");
    auto fields = section_match_fields(row);
    fields[1].score = 77;
    require(equal(score_section_match(row, groups, 1, &fields),
                  score_section_match(row, groups, 1, &fields, &work)), "caller-owned fields were ignored");
    require(work.metrics()["score_reuses"] == 0 && work.metrics()["uncacheable_calls"] == 3,
            "incompatible fields or groups were admitted");
}
void capacity_is_not_a_search_budget() {
    const auto groups = prepare_term_groups({{"alpha"}});
    for (std::size_t budget : {std::size_t{0}, std::size_t{16}, std::size_t{8192}, std::size_t{4U * 1024U * 1024U}}) {
        RequestScoring work(groups, budget, 2);
        for (int pass = 0; pass < 2; ++pass) for (int number = 0; number < 20; ++number) {
            SectionRow row; row.path = std::to_string(number); row.heading = "alpha";
            row.indexed_text.assign(10000, 'x');
            require(equal(score_section_match(row, groups, 1), score_section_match(row, groups, 1, nullptr, &work)),
                    "capacity exhaustion lost matching work");
        }
        const auto metrics = work.metrics();
        require(metrics["retained_entries"].get<std::size_t>() <= 2 &&
                metrics["retained_payload_bytes_estimate"].get<std::size_t>() <= budget,
                "request cache exceeded a retention bound");
        require(metrics["score_calls"] == 40, "capacity exhaustion skipped rows");
    }
    auto oversized = groups;
    oversized[0][0].phrase_keys.push_back(std::string(20000, 'x'));
    RequestScoring work(oversized, 8192);
    SectionRow row; row.heading = "alpha";
    require(equal(score_section_match(row, oversized, 1), score_section_match(row, oversized, 1, nullptr, &work)),
            "oversized key did not fall through");
    require(work.metrics()["retained_payload_bytes_estimate"] == 0 && work.metrics()["uncacheable_calls"] == 1,
            "group key allocation escaped the budget");
}
void independent_requests() {
    std::vector<std::future<void>> tasks;
    for (int worker = 0; worker < 8; ++worker) tasks.push_back(std::async(std::launch::async, [] {
        const auto groups = prepare_term_groups({{"alpha"}, {"beta"}});
        SectionRow row; row.heading = "alpha beta";
        for (int request = 0; request < 4; ++request) {
            RequestScoring work(groups);
            for (int threshold : {2, 1, 0})
                require(equal(score_section_match(row, groups, threshold),
                              score_section_match(row, groups, threshold, nullptr, &work)), "concurrent request changed scoring");
            require(work.metrics()["score_computations"] == 1 && work.metrics()["score_reuses"] == 2,
                    "scoring state leaked across requests");
        }
    }));
    for (auto& task : tasks) task.get();
}
}
int main() {
    try {
        threshold_and_row_identity(); aliases_and_prepared_fields();
        capacity_is_not_a_search_budget(); independent_requests();
        std::cout << "request scoring: 4 groups passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
