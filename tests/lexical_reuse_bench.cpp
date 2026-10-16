#include "context_internal.h"
#include "profile_quality.h"
#include "lexical_reuse_experiment.h"
#include <list>
#include <ctime>
#include <unordered_map>

namespace shin::context::experiment {
namespace {
struct Entry {
    std::string source;
    std::vector<std::string> terms;
    std::size_t bytes;
};
struct Cache {
    bool enabled = false;
    std::size_t limit = 32U * 1024U * 1024U;
    std::size_t bytes = 0, hits = 0, misses = 0, evictions = 0;
    std::list<Entry> lru;
    std::unordered_map<std::string, std::list<Entry>::iterator> lookup;
};
thread_local Cache cache;
void reset(bool enabled, std::size_t limit = 32U * 1024U * 1024U) {
    cache = Cache{};
    cache.enabled = enabled;
    cache.limit = limit;
}
Json metrics() {
    return {{"hits", cache.hits}, {"misses", cache.misses}, {"evictions", cache.evictions},
        {"entries", cache.lru.size()}, {"retained_payload_bytes_estimate", cache.bytes},
        {"payload_limit", cache.limit}};
}
}
std::vector<std::string> lexical_terms(std::string_view text) {
    if (!cache.enabled) return lexical_match_terms(text);
    // Exact owned text is the key. No hash-only hit, path or authority reuse.
    std::string key(text);
    if (const auto found = cache.lookup.find(key); found != cache.lookup.end()) {
        ++cache.hits;
        cache.lru.splice(cache.lru.begin(), cache.lru, found->second);
        return found->second->terms;
    }
    ++cache.misses;
    auto terms = lexical_match_terms(text);
    std::size_t bytes = 2 * (key.capacity() + sizeof(std::string)) + sizeof(Entry) +
                        terms.capacity() * sizeof(std::string);
    for (const auto& term : terms) bytes += term.capacity();
    if (bytes > cache.limit) return terms;
    while (!cache.lru.empty() &&
           (cache.bytes > cache.limit - bytes || cache.lru.size() >= 4096)) {
        cache.bytes -= cache.lru.back().bytes;
        cache.lookup.erase(cache.lru.back().source);
        cache.lru.pop_back();
        ++cache.evictions;
    }
    cache.lru.push_front({key, terms, bytes});
    cache.lookup.emplace(std::move(key), cache.lru.begin());
    cache.bytes += bytes;
    return terms;
}
}

namespace {
using namespace shin::context;
void require(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
Json suite_summary(const SuiteEvaluation& s) {
    return {{"total", s.total}, {"passed", s.passed}, {"failed", s.failed},
        {"query_total", s.query_total}, {"query_passed", s.query_passed},
        {"quality_failed_queries", s.quality_failed_queries},
        {"resource_failed_queries", s.resource_failed_queries}, {"failed_case_ids", s.failed_case_ids}};
}
void synthetic() {
    using namespace experiment;
    reset(true);
    for (const auto& text : {"CamelCase path/to/file.cpp", "alpha beta gamma", "", "alpha-beta",
                            "alpha beta gamma"})
        require(lexical_terms(text) == lexical_match_terms(text), "lexical oracle mismatch");
    require(cache.hits > 0, "no reuse tested");
    reset(true, 0);
    require(lexical_terms("hello world") == lexical_match_terms("hello world") && cache.bytes == 0,
            "zero-budget behavior");
    reset(true, 1024);
    for (int n = 0; n < 40; ++n) {
        const auto text = "term " + std::to_string(n);
        require(lexical_terms(text) == lexical_match_terms(text), "eviction oracle mismatch");
        require(cache.bytes <= cache.limit, "payload budget exceeded");
    }
    require(cache.evictions > 0, "no eviction tested");
    reset(false);
}
Json audit(RetrievalSession& session, bool enabled, std::size_t payload_limit) {
    experiment::reset(enabled, payload_limit);
    Json result = Json::array();
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
            result.push_back(session.brief(request));
        }
    }
    return result;
}
int benchmark(int argc, char** argv) {
    synthetic();
    if (argc == 1) {
        std::cout << Json({{"ok", true}, {"synthetic", true}}).dump() << '\n';
        return 0;
    }
    if (argc != 6 && argc != 7) {
        std::cerr << "usage: shin-lexical-reuse-bench ROOT OUTPUT PROFILE_DIR PROFILE baseline-first|candidate-first [payload-limit-bytes]\n";
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
    Json result{{"schema_version", "shin.lexical-reuse-experiment.v1"}, {"order", order},
                {"admission_ms", elapsed_ms_since(start)}, {"runs", Json::array()}};
    Json oracle;
    for (bool candidate : {order == "candidate-first", order != "candidate-first"}) {
        experiment::reset(candidate, payload_limit);
        start = SteadyClock::now();
        const auto cpu_start = std::clock();
        const auto suite = evaluate_profile_cases(session);
        const auto cpu_end = std::clock();
        const double duration = elapsed_ms_since(start);
        const auto summary = suite_summary(suite);
        require(suite.total > 0 && suite.failed == 0, "quality suite failed");
        if (oracle.is_null()) oracle = summary;
        else require(summary == oracle, "suite assertion parity failed");
        result["runs"].push_back({{"candidate", candidate}, {"cases_ms", duration},
            {"summary", summary}, {"cache", experiment::metrics()},
            {"process_cpu_ms", cpu_start == std::clock_t(-1) || cpu_end == std::clock_t(-1)
                ? Json(nullptr) : Json(1000.0 * (cpu_end - cpu_start) / CLOCKS_PER_SEC)}});
    }
    // Exact full answer parity (scores, ordering, provenance and diagnostics), outside timing.
    const auto baseline = audit(session, false, payload_limit);
    const auto candidate = audit(session, true, payload_limit);
    require(baseline == candidate, "full brief parity failed");
    result["exact_brief_queries"] = baseline.size();
    result["ok"] = true;
    result["process_peak_rss_bytes"] = process_peak_rss_bytes();
    result["boundary"] = "Optional binary only; production lexical matching has no cache branch. One immutable admitted session; exact-text vocabulary memo only, reset per lane. Timed complete eval includes assertions, excludes admission and full brief parity pass. Retained payload excludes container/allocator overhead; RSS includes both lanes and parity JSON. No full-refresh, Pack-worker, persistence or deployed speedup claim.";
    std::cout << result.dump(2) << '\n';
    return 0;
}
}
int main(int argc, char** argv) {
    try { return benchmark(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
