#include "context_internal.h"
#include "field_preparation_experiment.h"
#include "profile_quality.h"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace shin::context;
using namespace shin::context::experiment;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
Json suite_summary(const SuiteEvaluation& suite) {
    return {{"total", suite.total}, {"passed", suite.passed}, {"failed", suite.failed},
        {"query_total", suite.query_total}, {"query_passed", suite.query_passed},
        {"quality_failed_queries", suite.quality_failed_queries},
        {"resource_failed_queries", suite.resource_failed_queries},
        {"failed_case_ids", suite.failed_case_ids}};
}
Json full_brief_audit(RetrievalSession& session, bool enabled, std::size_t limit) {
    reset_fields(enabled, limit);
    Json results = Json::array();
    for (const auto& item : session.options().profile.eval_cases) {
        auto queries = item.query_variants;
        queries.insert(queries.begin(), item.query);
        for (const auto& query : queries) {
            QueryRequest request;
            request.query = query;
            request.terms = parse_terms(query);
            request.limit = item.limit > 0 ? item.limit : 8;
            request.per_doc_limit = item.per_doc_limit > 0 ? item.per_doc_limit : 2;
            apply_profile_query_expansion(session.options().profile, request);
            results.push_back(session.brief(request));
        }
    }
    return results;
}
struct ScoreCase {
    bool fact = false;
    SectionRow section;
    FactRow row_fact;
    PreparedTermGroups groups;
    int required = 0;
};
Json score_json(const MatchScore& score) {
    return {{"total", score.total}, {"matched_groups", score.matched_groups},
        {"components", score.components}, {"matched_fields", score.matched_fields},
        {"matched_group_indices", score.matched_group_indices}};
}
std::vector<Json> score_sequence(const std::vector<ScoreCase>& cases,
                                 const std::vector<std::size_t>& order, bool enabled) {
    reset_fields(enabled);
    std::vector<Json> result;
    result.reserve(order.size());
    for (const auto index : order) {
        const auto& item = cases[index];
        const auto score = item.fact ? score_fact_match(item.row_fact, item.groups, item.required)
                                     : score_section_match(item.section, item.groups, item.required);
        result.push_back(score_json(score));
    }
    return result;
}
void verify_score_parity() {
    SectionRow section;
    section.path = "SRC/Thing.CPP";
    section.document_id = "CamelCaseDoc";
    section.heading = "CamelCase alpha beta runtime";
    section.indexed_text = "alpha beta gamma feature";
    section.topics = {"runtime", "feature", "runtime"};
    section.anchor = "main";
    section.line = 12;
    section.level = 1;

    FactRow fact;
    fact.path = "src/Thing.cpp";
    fact.id = "fact-abc-123";
    fact.record_type = "reference";
    fact.title = "Compiler issue";
    fact.summary = "Runtime migration feature";
    fact.identifier_terms = {"ABC-123", "owner:ABC-123"};
    fact.topics = {"runtime", "feature"};
    fact.line = 19;

    auto groups = [](std::initializer_list<std::initializer_list<std::string>> values) {
        std::vector<std::vector<std::string>> raw;
        for (const auto& group : values) raw.emplace_back(group);
        return prepare_term_groups(raw);
    };
    std::vector<ScoreCase> cases;
    cases.push_back({false, section, {}, groups({{"CamelCase"}}), 1});
    cases.push_back({false, section, {}, groups({{"camel case"}}), 1});
    cases.push_back({false, section, {}, groups({{"camelcase"}}), 1});
    cases.push_back({false, section, {}, groups({{"alpha beta"}}), 1});
    cases.push_back({false, section, {}, groups({{"beta alpha"}}), 1});
    auto prefix_on = groups({{"identi"}});
    prefix_on.at(0).at(0).allow_prefix = true;
    auto prefix_off = prefix_on;
    prefix_off.at(0).at(0).allow_prefix = false;
    SectionRow prefix_row = section;
    prefix_row.heading = "identity";
    cases.push_back({false, prefix_row, {}, prefix_on, 1});
    cases.push_back({false, prefix_row, {}, prefix_off, 1});
    cases.push_back({false, section, {}, groups({{"src/thing.cpp"}}), 1});
    cases.push_back({true, {}, fact, groups({{"ABC-123"}}), 1});
    cases.push_back({true, {}, fact, groups({{"ABC"}, {"runtime"}}), 2});
    cases.push_back({true, {}, fact, groups({{"owner:ABC-123"}}), 1});
    cases.push_back({false, section, {}, groups({{"runtime"}}), 1});
    cases.push_back({false, section, {}, groups({{"runtime"}, {"runtime"}}), 2});
    cases.push_back({false, section, {}, groups({{"feature"}, {"alpha"}}), 2});
    cases.push_back({false, section, {}, groups({{"missing"}}), 1});

    std::vector<std::size_t> forward(cases.size());
    for (std::size_t i = 0; i < forward.size(); ++i) forward[i] = i;
    auto reverse = forward;
    std::reverse(reverse.begin(), reverse.end());
    const auto fresh_forward = score_sequence(cases, forward, false);
    require(score_sequence(cases, forward, true) == fresh_forward,
            "cached section/fact MatchScore sequence changed");
    const auto fresh_reverse = score_sequence(cases, reverse, false);
    require(score_sequence(cases, reverse, true) == fresh_reverse,
            "cached reverse MatchScore sequence changed");
}

void synthetic() {
    verify_score_parity();
    reset_fields(true);
    SectionRow section;
    section.path = "src/one.cpp";
    section.document_id = "doc-one";
    section.anchor = "anchor";
    section.line = 8;
    section.heading = "First heading";
    auto first = prepared_section_fields(section);
    require(first.get() && first.get()->at(0).text == "doc-one", "section source was not copied");
    section.heading = "mutated caller copy";
    auto same = prepared_section_fields(SectionRow{
        .document_id = "doc-one", .path = "src/one.cpp", .heading = "First heading",
        .anchor = "anchor", .line = 8});
    require(same.get() && field_metrics()["hits"] == 1, "row cache hit missing");
    SectionRow changed;
    changed.path = "src/one.cpp"; changed.document_id = "doc-one"; changed.anchor = "anchor";
    changed.line = 8; changed.heading = "Changed heading";
    auto replaced = prepared_section_fields(changed);
    require(replaced.get()->at(1).text == "Changed heading" && field_metrics()["invalidations"] == 1,
            "same-key changed row was reused");

    FactRow fact;
    fact.path = "src/one.cpp"; fact.id = "fact-one"; fact.record_type = "decision";
    fact.line = 8; fact.title = "Fact title"; fact.identifier_terms = {"ABC-42"};
    fact.topics = {"runtime"};
    auto fact_fields = prepared_fact_fields(fact);
    require(fact_fields.get() && fact_fields.get()->at(0).name == "title" &&
            fact_fields.get()->at(8).text == "ABC-42", "fact fields/source ownership failed");
    auto changed_identifiers = fact; changed_identifiers.identifier_terms = {"XYZ-99"};
    auto changed_fact = prepared_fact_fields(changed_identifiers);
    require(changed_fact.get()->at(8).text == "XYZ-99" && field_metrics()["invalidations"] == 2,
            "fact identifier change did not invalidate");
    auto changed_topics = changed_identifiers; changed_topics.topics = {"storage"};
    auto topic_fields = prepared_fact_fields(changed_topics);
    require(topic_fields.get()->back().text == "storage" && field_metrics()["invalidations"] == 3,
            "fact topic change did not invalidate");
    auto section_same_identity = prepared_section_fields(SectionRow{
        .document_id = "doc-one", .path = "src/one.cpp", .anchor = "anchor", .line = 8});
    require(section_same_identity.get() && section_same_identity.get()->at(0).name == "document_id",
            "section/fact identity namespaces collided");

    // Force the scorer to grow both vocabulary and normalized text beyond the
    // remaining cache budget. Final lease release must account and evict it.
    SectionRow lazy;
    lazy.path = "lazy.cpp"; lazy.document_id = "lazy"; lazy.anchor = "a";
    for (int i = 0; i < 300; ++i) lazy.heading += "alpha, beta; gamma delta; ";
    const std::size_t lazy_limit = lazy.heading.size() * 3 / 2;
    reset_fields(true, lazy_limit);
    {
        auto lease = prepared_section_fields(lazy);
        require(lease.get() && field_metrics()["entries"] == 1 &&
                field_metrics()["retained_payload_bytes_estimate"] <= lazy_limit,
                "lazy-growth fixture did not begin cached within budget");
        const auto phrase_groups = prepare_term_groups({{"alpha beta gamma"}});
        (void)field_match_kind(lease.get()->at(1), phrase_groups.at(0).at(0));
        require(lease.get()->at(1).lexical_ready && lease.get()->at(1).normalized_ready,
                "scorer did not allocate lazy vocabulary and normalized text");
    }
    const auto after_lazy_growth = field_metrics();
    require(after_lazy_growth["lazy_growth"].get<std::size_t>() > 0 &&
            after_lazy_growth["evictions"].get<std::size_t>() == 1 &&
            after_lazy_growth["entries"] == 0 &&
            after_lazy_growth["retained_payload_bytes_estimate"] <= lazy_limit,
            "lazy growth failed to trigger bounded eviction on lease release");

    reset_fields(true, 0);
    auto oversized_row = lazy;
    oversized_row.heading.assign(256, 'x');
    auto transient = prepared_section_fields(oversized_row);
    require(transient.get() && field_metrics()["entries"] == 0 &&
            field_metrics()["active_transient_leases"] == 1,
            "zero/oversize budget did not use a transient lease");
    transient = {};
    require(field_metrics()["active_transient_leases"] == 0, "transient lease did not close");

    reset_fields(true, 12000);
    for (int i = 0; i < 80; ++i) {
        SectionRow row;
        row.path = "evict/" + std::to_string(i) + ".cpp";
        row.document_id = std::to_string(i);
        row.heading.assign(180, static_cast<char>('a' + (i % 20)));
        auto lease = prepared_section_fields(row);
        require(lease.get(), "eviction fixture unexpectedly bypassed cache");
    }
    const auto after_eviction = field_metrics();
    require(after_eviction["evictions"].get<std::size_t>() > 0 &&
            after_eviction["retained_payload_bytes_estimate"].get<std::size_t>() <= 12000,
            "LRU eviction/budget failed");

    // Evict a row while its lease is active, then reset the epoch. The lease
    // must continue to own its field views, and release cannot alter new metrics.
    reset_fields(true, 12000);
    PreparedFieldsLease survivor;
    SectionRow survivor_probe;
    survivor_probe.path = "leased.cpp"; survivor_probe.document_id = "leased";
    survivor_probe.heading.assign(2400, 'x');
    {
        SectionRow row = survivor_probe;
        survivor = prepared_section_fields(row);
    }
    require(survivor.get(), "active survivor was not initially retained");
    for (int i = 0; i < 80; ++i) {
        SectionRow row;
        row.path = "evict-active/" + std::to_string(i) + ".cpp";
        row.document_id = std::to_string(i);
        row.heading.assign(180, static_cast<char>('a' + (i % 20)));
        auto lease = prepared_section_fields(row);
    }
    const auto after_active_eviction = field_metrics();
    const auto misses_before_probe = after_active_eviction["misses"].get<std::size_t>();
    auto eviction_probe = prepared_section_fields(survivor_probe);
    require(field_metrics()["misses"].get<std::size_t>() == misses_before_probe + 1 &&
            after_active_eviction["evictions"].get<std::size_t>() > 0 &&
            survivor.get()->at(1).text == std::string(2400, 'x'),
            "active lease did not survive after its cache entry was evicted");
    eviction_probe = {};
    const auto* stable_data = survivor.get()->at(1).text.data();
    reset_fields(true, 12000);
    const auto new_epoch_metrics = field_metrics();
    require(survivor.get() && survivor.get()->at(1).text.data() == stable_data &&
            survivor.get()->at(1).text == std::string(2400, 'x'),
            "lease became stale after reset/source destruction");
    survivor = {};
    require(field_metrics() == new_epoch_metrics,
            "old-epoch lease release changed new cache metrics");
    // Reset detaches cached ownership while a pinned entry remains usable;
    // releasing that old lease cannot mutate the new epoch.
    reset_fields(true, 12000);
    PreparedFieldsLease pinned_across_reset;
    {
        SectionRow row;
        row.path = "pinned-reset.cpp"; row.document_id = "pinned";
        row.heading = "still owned";
        pinned_across_reset = prepared_section_fields(row);
    }
    require(field_metrics()["entries"] == 1, "reset fixture was not cached");
    reset_fields(true, 12000);
    const auto detached_epoch_metrics = field_metrics();
    require(detached_epoch_metrics["entries"] == 0 && pinned_across_reset.get() &&
            pinned_across_reset.get()->at(1).text == "still owned",
            "reset did not detach cached ownership while preserving a lease");
    pinned_across_reset = {};
    require(field_metrics() == detached_epoch_metrics,
            "detached lease changed metrics in the new epoch");
    reset_fields(false);
    require(!prepared_section_fields(SectionRow{}).get(), "disabled baseline was not empty");
}
int benchmark(int argc, char** argv) {
    synthetic();
    if (argc == 1) {
        std::cout << Json({{"ok", true}, {"synthetic", true}}).dump() << '\n';
        return 0;
    }
    if (argc != 6 && argc != 7) {
        std::cerr << "usage: shin-field-preparation-bench ROOT OUTPUT PROFILE_DIR PROFILE baseline-first|candidate-first [payload-limit-bytes]\n";
        return 2;
    }
    const std::string order = argv[5];
    require(order == "baseline-first" || order == "candidate-first", "invalid order");
    const std::size_t payload_limit = argc == 7 ? std::stoull(argv[6]) : 32U * 1024U * 1024U;
    require(payload_limit <= 64U * 1024U * 1024U, "payload limit exceeds experiment cap");
    RuntimeOptions options;
    options.root = fs::canonical(argv[1]);
    options.out_root = fs::canonical(argv[2]);
    options.profile_dir = fs::canonical(argv[3]);
    options.profile = make_profile(argv[4], options.profile_dir);
    options.force_index = true;
    auto start = SteadyClock::now();
    RetrievalSession session(options, true);
    Json result{{"schema_version", "shin.field-preparation-experiment.v1"}, {"order", order},
                {"admission_ms", elapsed_ms_since(start)}, {"runs", Json::array()}};
    Json oracle;
    for (bool candidate : {order == "candidate-first", order != "candidate-first"}) {
        reset_fields(candidate, payload_limit);
        start = SteadyClock::now();
        const auto cpu_start = std::clock();
        const auto suite = evaluate_profile_cases(session);
        const auto cases_ms = elapsed_ms_since(start);
        const auto rss_after_eval = process_peak_rss_bytes();
        const auto cpu_end = std::clock();
        const auto summary = suite_summary(suite);
        require(suite.total > 0 && suite.failed == 0, "quality suite failed");
        if (oracle.is_null()) oracle = summary;
        else require(summary == oracle, "suite assertion parity failed");
        const auto cache_metrics = field_metrics();
        require(cache_metrics["active_transient_leases"].get<std::size_t>() == 0,
                "timed suite left a transient field lease active");
        require(cache_metrics["entries"].get<std::size_t>() <=
                    cache_metrics["entry_limit"].get<std::size_t>(),
                "timed suite exceeded the field entry limit");
        require(cache_metrics["retained_payload_bytes_estimate"].get<std::size_t>() <= payload_limit,
                "timed suite exceeded the retained field payload limit");
        const auto cleanup_start = SteadyClock::now();
        reset_fields(false);
        const auto cleanup_ms = elapsed_ms_since(cleanup_start);
        result["runs"].push_back({{"candidate", candidate},
            {"cases_ms", cases_ms}, {"summary", summary}, {"process_peak_rss_after_eval_bytes", rss_after_eval}, {"cache", cache_metrics},
            {"cleanup_ms", cleanup_ms},
            {"process_cpu_ms", cpu_start == std::clock_t(-1) || cpu_end == std::clock_t(-1)
                ? Json(nullptr) : Json(1000.0 * (cpu_end - cpu_start) / CLOCKS_PER_SEC)}});
    }
    const auto baseline = full_brief_audit(session, false, payload_limit);
    const auto candidate = full_brief_audit(session, true, payload_limit);
    require(baseline == candidate, "full brief parity failed");
    result["exact_brief_queries"] = baseline.size();
    result["ok"] = true;
    result["process_peak_rss_bytes"] = process_peak_rss_bytes();
    result["boundary"] = "Optional binary only; production has no preparation-cache branch. One immutable admitted session; exact full-row confirmation and per-lane cache reset. Timed complete eval includes assertions, excludes admission and exact brief parity pass. Retained payload estimates owned row/field/key buffers and excludes allocator/container-node overhead; transient and evicted-but-leased entries may exceed that cache cap until lease release. Identity construction still allocates its short key. RSS includes both lanes and parity JSON. No full-refresh, persistence, or deployed speedup claim.";
    std::cout << result.dump(2) << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try { return benchmark(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
